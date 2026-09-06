#include "atlas/app.hpp"
#include "atlas/platform/utf.hpp"
#include "atlas/terminal/ui.hpp"

#include <Windows.h>

#include <string>
#include <string_view>
#include <vector>
#include <exception>

namespace {
struct ConsoleScope {
    UINT input_page{GetConsoleCP()}, output_page{GetConsoleOutputCP()};
    DWORD output_mode{}, error_mode{}, input_mode{};
    bool has_output{GetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), &output_mode) != 0};
    bool has_error{GetConsoleMode(GetStdHandle(STD_ERROR_HANDLE), &error_mode) != 0};
    bool has_input{GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), &input_mode) != 0};
    ~ConsoleScope() {
        if (input_page) SetConsoleCP(input_page);
        if (output_page) SetConsoleOutputCP(output_page);
        if (has_output) SetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), output_mode);
        if (has_error) SetConsoleMode(GetStdHandle(STD_ERROR_HANDLE), error_mode);
        if (has_input) SetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), input_mode);
    }
};
}

int wmain(const int argc, wchar_t* argv[]) {
    ConsoleScope console;
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    std::vector<std::string> owned;
    owned.reserve(static_cast<std::size_t>((argc > 0) ? argc - 1 : 0));
    for (int index = 1; index < argc; ++index) {
        auto argument = atlas::win::narrow(argv[index]);
        if (!argument) {
            atlas::terminal::Ui ui({});
            ui.error(argument.error());
            return 2;
        }
        owned.push_back(std::move(*argument));
    }
    std::vector<std::string_view> views;
    views.reserve(owned.size());
    for (const auto& argument : owned) views.push_back(argument);
    try {
        return atlas::run(views);
    } catch (const std::exception& exception) {
        atlas::terminal::Ui ui({});
        ui.error("Atlas could not complete the command", exception.what());
        return 1;
    }
}
