#include "atlas/platform/process.hpp"
#include "atlas/platform/handle.hpp"

#include "atlas/platform/utf.hpp"

#include <Windows.h>

#include <array>
#include <atomic>
#include <chrono>
#include <thread>
#include <utility>
#include <vector>

namespace atlas::win {
namespace {

struct Pipe {
    UniqueHandle read;
    UniqueHandle write;
};

// WinAPI calls console handlers on a separate thread. Only communicate an epoch here;
// the foreground runner performs cleanup on its owning thread.
std::atomic_uint console_interrupts{};
BOOL WINAPI interrupt_process(const DWORD event) {
    if (event != CTRL_C_EVENT && event != CTRL_BREAK_EVENT) return FALSE;
    console_interrupts.fetch_add(1, std::memory_order_relaxed);
    return TRUE;
}

struct ConsoleInterrupt {
    unsigned epoch{console_interrupts.load(std::memory_order_relaxed)};
    bool registered{SetConsoleCtrlHandler(interrupt_process, TRUE) != 0};
    ~ConsoleInterrupt() { if (registered) SetConsoleCtrlHandler(interrupt_process, FALSE); }
    bool requested() const { return epoch != console_interrupts.load(std::memory_order_relaxed); }
};

Result<Pipe> make_pipe() {
    SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE read = nullptr;
    HANDLE write = nullptr;
    if (!CreatePipe(&read, &write, &attributes, 0)) {
        const auto code = GetLastError();
        return std::unexpected(Error{ErrorKind::windows, "Could not create a process pipe", error_message(code), code});
    }
    Pipe pipe{UniqueHandle(read), UniqueHandle(write)};
    if (!SetHandleInformation(pipe.read.get(), HANDLE_FLAG_INHERIT, 0)) {
        const auto code = GetLastError();
        return std::unexpected(Error{ErrorKind::windows, "Could not configure a process pipe", error_message(code), code});
    }
    return pipe;
}

std::string read_pipe(HANDLE handle, const std::function<void(std::string_view)>& callback, const bool collect = true) {
    std::string output;
    std::array<char, 16 * 1024> buffer{};
    DWORD count = 0;
    while (ReadFile(handle, buffer.data(), static_cast<DWORD>(buffer.size()), &count, nullptr) && count != 0) {
        if (collect) output.append(buffer.data(), count);
        if (callback) callback(std::string_view(buffer.data(), count));
    }
    return output;
}

std::wstring command_line(const std::filesystem::path& executable, const std::span<const std::wstring> arguments) {
    auto line = quote_argument(executable.native());
    for (const auto& argument : arguments) {
        line.push_back(L' ');
        line += quote_argument(argument);
    }
    return line;
}

Result<PROCESS_INFORMATION> create_process(
    const std::filesystem::path& executable,
    const std::span<const std::wstring> arguments,
    STARTUPINFOW& startup,
    const bool inherit_handles,
    const DWORD flags,
    const std::optional<std::filesystem::path>& working_directory) {
    auto line = command_line(executable, arguments);
    const wchar_t* directory = working_directory ? working_directory->c_str() : nullptr;
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable.c_str(), line.data(), nullptr, nullptr, inherit_handles, flags,
                        nullptr, directory, &startup, &process)) {
        const auto code = GetLastError();
        auto executable_utf8 = narrow(executable.native()).value_or("<invalid path>");
        return std::unexpected(Error{ErrorKind::process,
            "Could not start '" + executable_utf8 + "'", error_message(code), code});
    }
    return process;
}

} // namespace

std::wstring quote_argument(const std::wstring_view argument) {
    if (argument.empty()) {
        return L"\"\"";
    }
    if (argument.find_first_of(L" \t\n\v\"") == std::wstring_view::npos) {
        return std::wstring(argument);
    }

    std::wstring quoted;
    quoted.push_back(L'\"');
    std::size_t backslashes = 0;
    for (const auto character : argument) {
        if (character == L'\\') {
            ++backslashes;
            continue;
        }
        if (character == L'\"') {
            quoted.append(backslashes * 2 + 1, L'\\');
            quoted.push_back(L'\"');
        } else {
            quoted.append(backslashes, L'\\');
            quoted.push_back(character);
        }
        backslashes = 0;
    }
    quoted.append(backslashes * 2, L'\\');
    quoted.push_back(L'\"');
    return quoted;
}

Result<std::filesystem::path> find_executable(const std::wstring_view executable) {
    if (executable.empty()) {
        return std::unexpected(Error{ErrorKind::usage, "Executable name cannot be empty"});
    }
    std::filesystem::path candidate(executable);
    if (candidate.has_parent_path() || candidate.has_root_path()) {
        std::error_code error;
        const auto absolute = std::filesystem::absolute(candidate, error);
        if (!error && std::filesystem::is_regular_file(absolute, error)) {
            return absolute;
        }
        auto name = narrow(executable).value_or("<invalid path>");
        return std::unexpected(Error{ErrorKind::not_found, "Executable was not found: " + name});
    }

    std::wstring owned(executable);
    const auto required = SearchPathW(nullptr, owned.c_str(), L".exe", 0, nullptr, nullptr);
    if (required == 0) {
        auto name = narrow(executable).value_or("<invalid name>");
        return std::unexpected(Error{ErrorKind::not_found, "'" + name + "' was not found in PATH"});
    }
    std::wstring path(static_cast<std::size_t>(required) + 1, L'\0');
    const auto written = SearchPathW(nullptr, owned.c_str(), L".exe", static_cast<DWORD>(path.size()), path.data(), nullptr);
    if (written == 0 || written >= path.size()) {
        const auto code = GetLastError();
        return std::unexpected(Error{ErrorKind::windows, "Could not resolve executable", error_message(code), code});
    }
    path.resize(written);
    return std::filesystem::path(path);
}

Result<ProcessResult> execute(const std::wstring_view executable,
                              const std::span<const std::wstring> arguments,
                              const ProcessOptions& options) {
    auto resolved = find_executable(executable);
    if (!resolved) {
        return std::unexpected(resolved.error());
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    Pipe stdout_pipe;
    Pipe stderr_pipe;
    if (options.capture_output) {
        auto out = make_pipe();
        if (!out) return std::unexpected(out.error());
        auto err = make_pipe();
        if (!err) return std::unexpected(err.error());
        stdout_pipe = std::move(*out);
        stderr_pipe = std::move(*err);
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        startup.hStdOutput = stdout_pipe.write.get();
        startup.hStdError = stderr_pipe.write.get();
    }

    DWORD flags = CREATE_UNICODE_ENVIRONMENT;
    if (options.hidden) flags |= CREATE_NO_WINDOW;
    ConsoleInterrupt interrupt;
    auto created = create_process(*resolved, arguments, startup, options.capture_output, flags, options.working_directory);
    if (!created) return std::unexpected(created.error());

    UniqueHandle process(created->hProcess);
    UniqueHandle thread(created->hThread);
    stdout_pipe.write.reset();
    stderr_pipe.write.reset();

    ProcessResult result;
    std::jthread stdout_reader;
    std::jthread stderr_reader;
    if (options.capture_output) {
        stdout_reader = std::jthread([&] { result.stdout_text = read_pipe(stdout_pipe.read.get(), options.on_stdout, options.collect_stdout); });
        stderr_reader = std::jthread([&] { result.stderr_text = read_pipe(stderr_pipe.read.get(), options.on_stderr); });
    }

    bool cancelled = false;
    for (;;) {
        const auto wait = WaitForSingleObject(process.get(), 25);
        if (wait == WAIT_OBJECT_0) break;
        if (wait == WAIT_FAILED) {
            const auto code = GetLastError();
            return std::unexpected(Error{ErrorKind::windows, "Could not wait for process", error_message(code), code});
        }
        if (options.stop_token.stop_requested() || interrupt.requested()) {
            cancelled = true;
            TerminateProcess(process.get(), ERROR_CANCELLED);
            WaitForSingleObject(process.get(), INFINITE);
            break;
        }
    }

    DWORD exit_code = 0;
    if (!GetExitCodeProcess(process.get(), &exit_code)) {
        const auto code = GetLastError();
        return std::unexpected(Error{ErrorKind::windows, "Could not read process exit code", error_message(code), code});
    }
    result.exit_code = exit_code;
    if (cancelled || interrupt.requested()) {
        cancelled = true;
        if (stdout_reader.joinable()) CancelSynchronousIo(stdout_reader.native_handle());
        if (stderr_reader.joinable()) CancelSynchronousIo(stderr_reader.native_handle());
    }
    if (stdout_reader.joinable()) stdout_reader.join();
    if (stderr_reader.joinable()) stderr_reader.join();
    if (cancelled) {
        return std::unexpected(Error{ErrorKind::process, "Process was cancelled"});
    }
    return result;
}

Result<std::uint32_t> spawn(const std::wstring_view executable,
                            const std::span<const std::wstring> arguments,
                            const bool hidden) {
    return spawn(executable, arguments, SpawnOptions{.hidden = hidden});
}

Result<std::uint32_t> spawn(const std::wstring_view executable,
                            const std::span<const std::wstring> arguments,
                            const SpawnOptions& options) {
    if (options.hidden && options.new_console) {
        return std::unexpected(Error{ErrorKind::usage, "A process cannot be hidden and request a new console"});
    }
    auto resolved = find_executable(executable);
    if (!resolved) return std::unexpected(resolved.error());
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    DWORD flags = CREATE_UNICODE_ENVIRONMENT | CREATE_NEW_PROCESS_GROUP;
    if (options.hidden) flags |= CREATE_NO_WINDOW;
    if (options.new_console) flags |= CREATE_NEW_CONSOLE;
    auto created = create_process(*resolved, arguments, startup, false, flags, options.working_directory);
    if (!created) return std::unexpected(created.error());
    UniqueHandle process(created->hProcess);
    UniqueHandle thread(created->hThread);
    return created->dwProcessId;
}

} // namespace atlas::win
