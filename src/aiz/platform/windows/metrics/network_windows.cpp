#include <aiz/platform/metrics/network.h>

#define WIN32_LEAN_AND_MEAN
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#include <windows.h>
#include <iphlpapi.h>
#include <netioapi.h>

#include <string>
#include <vector>

namespace aiz::platform {

static std::string wideToUtf8(const wchar_t* s) {
  if (!s) return {};
  int len = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
  if (len <= 1) return {};
  std::string out(len - 1, '\0');
  WideCharToMultiByte(CP_UTF8, 0, s, -1, out.data(), len, nullptr, nullptr);
  return out;
}

std::optional<NetworkCounters> readNetworkCounters(const std::string& interfaceFilter) {
#if defined(MIB_IF_TABLE2)
  MIB_IF_TABLE2* table = nullptr;
  if (GetIfTable2(&table) != NO_ERROR) return std::nullopt;

  NetworkCounters result{};
  for (ULONG i = 0; i < table->NumEntries; ++i) {
    const MIB_IF_ROW2& row = table->Table[i];

    if (row.Type == IF_TYPE_SOFTWARE_LOOPBACK) continue;
    if (row.OperStatus != IfOperStatusUp) continue;

    if (!interfaceFilter.empty()) {
      const std::string alias = wideToUtf8(row.Alias);
      const std::string desc = wideToUtf8(row.Description);
      if (alias.rfind(interfaceFilter, 0) != 0 && desc.rfind(interfaceFilter, 0) != 0) continue;
    }

    result.rxBytes += static_cast<std::uint64_t>(row.InOctets);
    result.txBytes += static_cast<std::uint64_t>(row.OutOctets);
  }

  FreeMibTable(table);
  return result;
#else
  ULONG size = 0;
  if (GetIfTable(nullptr, &size, FALSE) != ERROR_INSUFFICIENT_BUFFER) return std::nullopt;

  std::vector<std::uint8_t> buffer(size);
  auto* table = reinterpret_cast<MIB_IFTABLE*>(buffer.data());
  if (GetIfTable(table, &size, FALSE) != NO_ERROR) return std::nullopt;

  NetworkCounters result{};
  for (ULONG i = 0; i < table->dwNumEntries; ++i) {
    const MIB_IFROW& row = table->table[i];

    if (row.dwType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
    if (row.dwOperStatus != IF_OPER_STATUS_OPERATIONAL) continue;

    if (!interfaceFilter.empty()) {
      std::string desc(reinterpret_cast<const char*>(row.bDescr), row.dwDescrLen);
      if (desc.rfind(interfaceFilter, 0) != 0) continue;
    }

    result.rxBytes += static_cast<std::uint64_t>(row.dwInOctets);
    result.txBytes += static_cast<std::uint64_t>(row.dwOutOctets);
  }

  return result;
#endif
}

}  // namespace aiz::platform
