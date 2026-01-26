#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace aiz {

struct AdlPciLocation {
  std::uint32_t bus = 0;
  std::uint32_t device = 0;
  std::uint32_t function = 0;
};

// Windows-only: identifies a DXGI adapter instance.
// Matches the Windows LUID layout (HighPart/LowPart).
struct AdlAdapterLuid {
  std::uint32_t lowPart = 0;
  std::int32_t highPart = 0;
};

struct AdlPcieLink {
  unsigned int width = 0;         // lanes (e.g. 16 for x16)
  unsigned int generation = 0;    // PCIe gen (e.g. 4 for Gen4)
  unsigned int maxWidth = 0;
  unsigned int maxGeneration = 0;
};

struct AdlAvailability {
  bool available = false;
  // Human-readable backend hint, e.g. "ADL2" or "ADL".
  std::string backend;
  // Loaded DLL name if known (e.g. "atiadlxx.dll").
  std::string dll;
};

// Windows-only: checks whether AMD ADL can be loaded/initialized.
// On non-Windows platforms this returns {false, "", ""}.
AdlAvailability adlAvailability();

// Returns a multi-line human-readable diagnostics string for ADL.
// Safe to call even if ADL isn't present.
std::string adlDiagnostics();

// Windows-only: prints PCIe link diagnostics using Windows device properties
// (SetupAPI) and DXGI adapter enumeration. Safe to call even if ADL isn't present.
std::string pcieDiagnostics();

// Windows-only: prints ADLX runtime diagnostics (loads amdadlx64.dll and lists exported symbols).
// This helps confirm ADLX is installed and discover the correct initialization entry points.
std::string adlxDiagnostics();

// Windows-only: returns a short user-facing reason string when PCIe link info is
// unavailable for an AMD GPU. Returns empty string when no note is available.
//
// Examples:
// - "integrated (no PCIe)"
// - "ADL missing"
// - "ADL unavailable"
std::string amdPcieLinkNoteForDxgi(const std::optional<AdlAdapterLuid>& adapterLuid);

enum class AdlStatus {
  Available,
  MissingDll,   // atiadlxx/atiadlxy not present
  Unusable,     // present but missing required symbols or init failed
};

// Best-effort ADL status (Windows-only). On non-Windows this returns MissingDll.
AdlStatus adlStatus();

// Windows-only: runtime-loads AMD ADL (atiadlxx/atiadlxy) and queries PCIe link info.
// Returns nullopt when ADL isn't present/usable.
std::optional<AdlPcieLink> readAdlPcieLinkForDxgi(const std::string& dxgiName,
                                                 const std::optional<AdlAdapterLuid>& adapterLuid,
                                                 const std::optional<unsigned int>& amdOrdinal,
                                                 const std::optional<AdlPciLocation>& pciLoc);

}  // namespace aiz
