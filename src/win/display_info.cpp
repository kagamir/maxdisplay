#include "display_info.hpp"

#include <algorithm>
#include <iostream>
#include <limits>
#include <memory>
#include <utility>

namespace win_info {
namespace {

std::wstring numeric_address(const sockaddr* address) {
    wchar_t text[INET6_ADDRSTRLEN]{};
    if (address->sa_family == AF_INET) {
        auto ip = reinterpret_cast<const sockaddr_in*>(address);
        if (InetNtopW(AF_INET, &ip->sin_addr, text, INET6_ADDRSTRLEN)) return text;
    } else if (address->sa_family == AF_INET6) {
        auto ip = reinterpret_cast<const sockaddr_in6*>(address);
        if (InetNtopW(AF_INET6, &ip->sin6_addr, text, INET6_ADDRSTRLEN)) {
            return ip->sin6_scope_id ? std::wstring(text) + L"%" + std::to_wstring(ip->sin6_scope_id) : text;
        }
    }
    return {};
}

enum class RouteType { default_route, via_gateway, connected_subnet, no_route, loopback };
using Priority = std::pair<RouteType, std::uint64_t>;

Priority route_priority(const IP_ADAPTER_ADDRESSES& adapter, int family, const MIB_IPFORWARD_ROW2* routes, ULONG count) {
    if (adapter.IfType == IF_TYPE_SOFTWARE_LOOPBACK) return {RouteType::loopback, 0};
    Priority best{RouteType::no_route, std::numeric_limits<std::uint64_t>::max()};
    const ULONG interface_metric = family == AF_INET ? adapter.Ipv4Metric : adapter.Ipv6Metric;
    for (ULONG i = 0; i < count; ++i) {
        const auto& r = routes[i];
        const auto& dst = r.DestinationPrefix;
        if (r.InterfaceLuid.Value != adapter.Luid.Value || dst.Prefix.si_family != family ||
            r.Loopback || !r.ValidLifetime) continue;
        const bool has_gateway = family == AF_INET ? r.NextHop.Ipv4.sin_addr.s_addr != INADDR_ANY
                                                   : !IN6_IS_ADDR_UNSPECIFIED(&r.NextHop.Ipv6.sin6_addr);
        RouteType type;
        if (dst.PrefixLength == 0) {
            type = RouteType::default_route;
        } else if (has_gateway) {
            type = RouteType::via_gateway;
        } else {
            // Per-host/broadcast and multicast routes exist even on isolated adapters.
            const bool multicast = family == AF_INET ? (ntohl(dst.Prefix.Ipv4.sin_addr.s_addr) >> 28) == 0xe
                                                     : IN6_IS_ADDR_MULTICAST(&dst.Prefix.Ipv6.sin6_addr);
            if (multicast || dst.PrefixLength == (family == AF_INET ? 32 : 128)) continue;
            type = RouteType::connected_subnet;
        }
        best = (std::min)(best, Priority{type, std::uint64_t{interface_metric} + r.Metric});
    }
    return best;
}

} // namespace

Binding resolve_binding(const std::string& host, bool ipv4_only) {
    addrinfo hints{};
    hints.ai_family = ipv4_only ? AF_INET : AF_UNSPEC;
    hints.ai_socktype = ipv4_only ? SOCK_DGRAM : SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    addrinfo* raw = nullptr;
    if (getaddrinfo(host.empty() ? nullptr : host.c_str(), nullptr, &hints, &raw) != 0)
        return {AF_UNSPEC, std::wstring(host.begin(), host.end())};
    std::unique_ptr<addrinfo, decltype(&freeaddrinfo)> addresses(raw, freeaddrinfo);
    const auto family = raw->ai_family;
    auto address = numeric_address(raw->ai_addr);
    if (address == L"0.0.0.0" || address == L"::") address.clear();
    return {family, std::move(address)};
}

std::vector<std::wstring> listening_addresses(const Binding& binding) {
    if (!binding.address.empty()) return {binding.address};

    constexpr ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    ULONG size = 15 * 1024;
    std::vector<unsigned char> buffer(size);
    ULONG result = ERROR_BUFFER_OVERFLOW;
    for (int attempt = 0; result == ERROR_BUFFER_OVERFLOW && attempt < 3; ++attempt) {
        buffer.resize(size);
        result = GetAdaptersAddresses(binding.family, flags, nullptr,
                                      reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &size);
    }
    if (result != NO_ERROR) return {};

    MIB_IPFORWARD_TABLE2* table = nullptr;
    GetIpForwardTable2(static_cast<ADDRESS_FAMILY>(binding.family), &table);
    std::unique_ptr<MIB_IPFORWARD_TABLE2, decltype(&FreeMibTable)> routes(table, FreeMibTable);
    return rank_addresses(reinterpret_cast<const IP_ADAPTER_ADDRESSES*>(buffer.data()),
                          routes ? routes->Table : nullptr, routes ? routes->NumEntries : 0);
}

std::vector<std::wstring> rank_addresses(const IP_ADAPTER_ADDRESSES* adapters,
                                         const MIB_IPFORWARD_ROW2* routes, ULONG count) {
    std::vector<std::pair<Priority, std::wstring>> ranked;
    for (auto adapter = adapters; adapter; adapter = adapter->Next) {
        if (adapter->OperStatus != IfOperStatusUp) continue;
        for (auto addr = adapter->FirstUnicastAddress; addr; addr = addr->Next) {
            if (addr->DadState != IpDadStatePreferred && addr->DadState != IpDadStateDeprecated) continue;
            auto sa = addr->Address.lpSockaddr;
            if (!sa || (sa->sa_family != AF_INET && sa->sa_family != AF_INET6)) continue;
            auto text = numeric_address(sa);
            if (!text.empty()) ranked.emplace_back(route_priority(*adapter, sa->sa_family, routes, count), std::move(text));
        }
    }
    std::sort(ranked.begin(), ranked.end());
    std::vector<std::wstring> ips;
    for (const auto& entry : ranked) {
        if (std::find(ips.begin(), ips.end(), entry.second) == ips.end()) ips.push_back(entry.second);
    }
    return ips;
}

SIZE monitor_pixels(HWND window) {
    MONITORINFOEXW monitor{};
    monitor.cbSize = sizeof(monitor);
    if (!GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor)) return {};
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    if (EnumDisplaySettingsW(monitor.szDevice, ENUM_CURRENT_SETTINGS, &mode))
        return {static_cast<LONG>(mode.dmPelsWidth), static_cast<LONG>(mode.dmPelsHeight)};
    return {monitor.rcMonitor.right - monitor.rcMonitor.left, monitor.rcMonitor.bottom - monitor.rcMonitor.top};
}

KeepAwake::KeepAwake() : previous_(SetThreadExecutionState(ES_CONTINUOUS | ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED)) {
    if (!previous_) std::cerr << "Windows could not keep the display and system awake.\n";
}

KeepAwake::~KeepAwake() {
    if (previous_) SetThreadExecutionState(previous_ | ES_CONTINUOUS);
}

} // namespace win_info
