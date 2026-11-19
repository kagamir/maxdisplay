#pragma once

#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>

#include <cstdint>
#include <string>
#include <vector>

namespace win_info {

// An empty address selects all usable unicast addresses of this family.
struct Binding {
    int family;
    std::wstring address;
};

Binding resolve_binding(const std::string& host, bool ipv4_only);
std::vector<std::wstring> listening_addresses(const Binding& binding);
std::vector<std::wstring> rank_addresses(const IP_ADAPTER_ADDRESSES* adapters,
                                         const MIB_IPFORWARD_ROW2* routes, ULONG count);
SIZE monitor_pixels(HWND window);

class KeepAwake {
public:
    KeepAwake();
    ~KeepAwake();
    KeepAwake(const KeepAwake&) = delete;
    KeepAwake& operator=(const KeepAwake&) = delete;
private:
    EXECUTION_STATE previous_;
};

} // namespace win_info
