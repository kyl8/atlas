#include "atlas/platform/service.hpp"
#include "atlas/platform/utf.hpp"

#include <Windows.h>

#include <utility>
#include <vector>

namespace atlas::win {
namespace {

class UniqueServiceHandle {
public:
    explicit UniqueServiceHandle(SC_HANDLE value = nullptr) noexcept : value_(value) {}
    ~UniqueServiceHandle() { if (value_) CloseServiceHandle(value_); }
    UniqueServiceHandle(const UniqueServiceHandle&) = delete;
    UniqueServiceHandle& operator=(const UniqueServiceHandle&) = delete;
    UniqueServiceHandle(UniqueServiceHandle&& other) noexcept : value_(std::exchange(other.value_, nullptr)) {}
    [[nodiscard]] SC_HANDLE get() const noexcept { return value_; }
private:
    SC_HANDLE value_{};
};

} // namespace

Result<ServiceInfo> query_service(const std::wstring_view name) {
    UniqueServiceHandle manager(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
    if (!manager.get()) {
        const auto code = GetLastError();
        return std::unexpected(Error{ErrorKind::windows, "Could not inspect Windows services", {}, code});
    }
    const std::wstring owned_name(name);
    UniqueServiceHandle service(OpenServiceW(manager.get(), owned_name.c_str(), SERVICE_QUERY_STATUS | SERVICE_QUERY_CONFIG));
    if (!service.get()) {
        const auto code = GetLastError();
        if (code == ERROR_SERVICE_DOES_NOT_EXIST) return ServiceInfo{};
        return std::unexpected(Error{ErrorKind::windows,
            "Could not inspect the '" + narrow(name).value_or("<invalid name>") + "' service", {}, code});
    }

    SERVICE_STATUS_PROCESS status{};
    DWORD bytes{};
    if (!QueryServiceStatusEx(service.get(), SC_STATUS_PROCESS_INFO,
                              reinterpret_cast<BYTE*>(&status), sizeof(status), &bytes)) {
        const auto code = GetLastError();
        return std::unexpected(Error{ErrorKind::windows, "Could not read the Everything service status", {}, code});
    }

    QueryServiceConfigW(service.get(), nullptr, 0, &bytes);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
        const auto code = GetLastError();
        return std::unexpected(Error{ErrorKind::windows, "Could not read the Everything service configuration", {}, code});
    }
    std::vector<std::byte> storage(bytes);
    auto* config = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(storage.data());
    if (!QueryServiceConfigW(service.get(), config, bytes, &bytes)) {
        const auto code = GetLastError();
        return std::unexpected(Error{ErrorKind::windows, "Could not read the Everything service configuration", {}, code});
    }
    return ServiceInfo{true, status.dwCurrentState == SERVICE_RUNNING, config->dwStartType == SERVICE_AUTO_START};
}

} // namespace atlas::win
