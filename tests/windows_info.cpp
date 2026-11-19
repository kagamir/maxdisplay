#include "win/display_info.hpp"

#include <array>
#include <iostream>
#include <stdexcept>

namespace {

void check_addresses() {
    std::array<IP_ADAPTER_ADDRESSES, 7> adapters{};
    std::array<IP_ADAPTER_UNICAST_ADDRESS, 7> unicast{};
    std::array<sockaddr_in, 7> addresses{};
    for (unsigned i = 0; i < adapters.size(); ++i) {
        adapters[i].Luid.Value = i + 1;
        adapters[i].OperStatus = IfOperStatusUp;
        adapters[i].Ipv4Metric = 10;
        adapters[i].Next = i + 1 < adapters.size() ? &adapters[i + 1] : nullptr;
        adapters[i].FirstUnicastAddress = &unicast[i];
        unicast[i].DadState = IpDadStatePreferred;
        unicast[i].Address.lpSockaddr = reinterpret_cast<sockaddr*>(&addresses[i]);
        addresses[i].sin_family = AF_INET;
        addresses[i].sin_addr.s_addr = htonl(0xc0000201 + i); // 192.0.2.1 to .7
    }
    adapters[0].IfType = IF_TYPE_SOFTWARE_LOOPBACK;
    adapters[3].Ipv4Metric = 40;
    adapters[5].OperStatus = IfOperStatusDown;

    std::array<MIB_IPFORWARD_ROW2, 5> routes{};
    // Competing default routes: .4's route metric is lower, .5's total metric wins.
    for (size_t i = 0; i < routes.size(); ++i) {
        auto& route = routes[i];
        route.InterfaceLuid = adapters[i + 2].Luid;
        route.DestinationPrefix.Prefix.Ipv4.sin_family = AF_INET;
        route.NextHop.Ipv4.sin_family = AF_INET;
        route.NextHop.Ipv4.sin_addr.s_addr = htonl(0xc00002fe);
        route.ValidLifetime = 3600;
    }
    routes[0].DestinationPrefix.PrefixLength = 8; // .3: routed through a gateway
    routes[2].Metric = 10;
    routes[4].DestinationPrefix.PrefixLength = 24; // .7: connected LAN
    routes[4].NextHop.Ipv4.sin_addr.s_addr = 0;
    auto ranked = win_info::rank_addresses(adapters.data(), routes.data(), static_cast<ULONG>(routes.size()));
    const std::vector<std::wstring> expected = {L"192.0.2.5", L"192.0.2.4", L"192.0.2.3",
                                                L"192.0.2.7", L"192.0.2.2", L"192.0.2.1"};
    if (ranked != expected) throw std::runtime_error("Incorrect address ordering by route and interface metric");

    auto binding = win_info::resolve_binding("127.0.0.1", false);
    if (win_info::listening_addresses(binding) != std::vector<std::wstring>{L"127.0.0.1"})
        throw std::runtime_error("Receiver advertised an address outside its binding");
    auto all = win_info::listening_addresses(win_info::resolve_binding("0.0.0.0", true));
    if (all.empty()) throw std::runtime_error("No IPv4 address returned from the Windows adapter list");
}

void check_power() {
    const auto previous = SetThreadExecutionState(ES_CONTINUOUS);
    if (!previous) throw std::runtime_error("Cannot inspect the test thread's power state");
    {
        win_info::KeepAwake guard;
        auto flags = SetThreadExecutionState(ES_CONTINUOUS);
        constexpr auto required = ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED;
        SetThreadExecutionState(flags | ES_CONTINUOUS);
        if ((flags & required) != required) throw std::runtime_error("Power request missing from receiver thread");
    }
    auto restored = SetThreadExecutionState(previous | ES_CONTINUOUS);
    if (restored & (ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED))
        throw std::runtime_error("Receiver failed to clear its power request");
}

} // namespace

int main() {
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return 1;
    int result = 0;
    try {
        check_addresses();
        check_power();
        HWND desktop = GetDesktopWindow();
        if (MonitorFromWindow(desktop, MONITOR_DEFAULTTONULL)) {
            auto size = win_info::monitor_pixels(desktop);
            if (size.cx <= 0 || size.cy <= 0) throw std::runtime_error("Missing display resolution");
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        result = 1;
    }
    WSACleanup();
    return result;
}
