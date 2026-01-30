#include <aiz/metrics/intel_igcl.h>
#include <aiz/metrics/windows_d3dkmt.h>

#if defined(_WIN32)

#include <mutex>

#if defined(AI_Z_HAS_IGCL_HEADERS) && (AI_Z_HAS_IGCL_HEADERS == 1)

// Provided by Intel IGCL SDK (headers) + its cApiWrapper.cpp (sources).
#include <igcl_api.h>

#include <algorithm>
#include <chrono>
#include <unordered_map>
#include <vector>
#include <sstream>
#endif

namespace aiz {
namespace {

#if defined(AI_Z_HAS_IGCL_HEADERS) && (AI_Z_HAS_IGCL_HEADERS == 1)

struct IgclSample {
  double lastTimestampS = -1.0;
  double lastGpuEnergyJ = -1.0;
  double lastGlobalActivityS = -1.0;
  double lastMediaActivityS = -1.0;
  // VRAM bandwidth counters (bytes).
  double lastVramReadBandwidthBytes = -1.0;
  double lastVramWriteBandwidthBytes = -1.0;
};

struct IgclDeviceInfo {
  ctl_device_adapter_handle_t handle = nullptr;
  ctl_device_adapter_properties_t props{};
};

struct IgclState {
  std::mutex mu;
  bool initTried = false;
  ctl_result_t initResult = CTL_RESULT_ERROR_NOT_INITIALIZED;
  ctl_api_handle_t api = nullptr;
  std::vector<IgclDeviceInfo> devices;
  std::unordered_map<ctl_device_adapter_handle_t, IgclSample> samples;
};

static IgclState& igclState() {
  static IgclState st;
  return st;
}

static std::optional<double> toDouble(const ctl_oc_telemetry_item_t& item) {
  if (!item.bSupported) return std::nullopt;
  switch (item.type) {
    case CTL_DATA_TYPE_INT8: return static_cast<double>(item.value.data8);
    case CTL_DATA_TYPE_UINT8: return static_cast<double>(item.value.datau8);
    case CTL_DATA_TYPE_INT16: return static_cast<double>(item.value.data16);
    case CTL_DATA_TYPE_UINT16: return static_cast<double>(item.value.datau16);
    case CTL_DATA_TYPE_INT32: return static_cast<double>(item.value.data32);
    case CTL_DATA_TYPE_UINT32: return static_cast<double>(item.value.datau32);
    case CTL_DATA_TYPE_INT64: return static_cast<double>(item.value.data64);
    case CTL_DATA_TYPE_UINT64: return static_cast<double>(item.value.datau64);
    case CTL_DATA_TYPE_FLOAT: return static_cast<double>(item.value.datafloat);
    case CTL_DATA_TYPE_DOUBLE: return static_cast<double>(item.value.datadouble);
    default: return std::nullopt;
  }
}

static const char* unitsName(ctl_units_t units) {
  switch (units) {
    case CTL_UNITS_FREQUENCY_MHZ: return "MHz";
    case CTL_UNITS_OPERATIONS_GTS: return "GT/s";
    case CTL_UNITS_OPERATIONS_MTS: return "MT/s";
    case CTL_UNITS_VOLTAGE_VOLTS: return "V";
    case CTL_UNITS_POWER_WATTS: return "W";
    case CTL_UNITS_TEMPERATURE_CELSIUS: return "C";
    case CTL_UNITS_ENERGY_JOULES: return "J";
    case CTL_UNITS_TIME_SECONDS: return "s";
    case CTL_UNITS_MEMORY_BYTES: return "B";
    case CTL_UNITS_ANGULAR_SPEED_RPM: return "RPM";
    case CTL_UNITS_POWER_MILLIWATTS: return "mW";
    case CTL_UNITS_PERCENT: return "%";
    case CTL_UNITS_MEM_SPEED_GBPS: return "GB/s";
    case CTL_UNITS_VOLTAGE_MILLIVOLTS: return "mV";
    case CTL_UNITS_BANDWIDTH_MBPS: return "MB/s";
    default: return "unknown";
  }
}

static const char* typeName(ctl_data_type_t type) {
  switch (type) {
    case CTL_DATA_TYPE_INT8: return "i8";
    case CTL_DATA_TYPE_UINT8: return "u8";
    case CTL_DATA_TYPE_INT16: return "i16";
    case CTL_DATA_TYPE_UINT16: return "u16";
    case CTL_DATA_TYPE_INT32: return "i32";
    case CTL_DATA_TYPE_UINT32: return "u32";
    case CTL_DATA_TYPE_INT64: return "i64";
    case CTL_DATA_TYPE_UINT64: return "u64";
    case CTL_DATA_TYPE_FLOAT: return "f32";
    case CTL_DATA_TYPE_DOUBLE: return "f64";
    case CTL_DATA_TYPE_STRING_ASCII: return "str8";
    case CTL_DATA_TYPE_STRING_UTF16: return "str16";
    case CTL_DATA_TYPE_STRING_UTF132: return "str32";
    default: return "unknown";
  }
}

static std::string formatTelemetryItem(const ctl_oc_telemetry_item_t& item) {
  std::ostringstream oss;
  oss << (item.bSupported ? "yes" : "no");
  if (!item.bSupported) return oss.str();
  oss << " type=" << typeName(item.type) << " units=" << unitsName(item.units);
  if (const auto v = toDouble(item)) {
    oss << " value=" << *v;
  }
  return oss.str();
}

static bool ensureIgclInitLocked(IgclState& st) {
  if (st.initTried) return st.api != nullptr;
  st.initTried = true;

  ctl_init_args_t init{};
  init.Size = sizeof(init);
  init.Version = 0;
  init.AppVersion = CTL_IMPL_VERSION;
  init.flags = static_cast<ctl_init_flags_t>(CTL_INIT_FLAG_USE_LEVEL_ZERO);
  init.ApplicationUID = {};

  st.initResult = ctlInit(&init, &st.api);
  if (st.initResult != CTL_RESULT_SUCCESS || !st.api) {
    // Best-effort fallback: retry without Level Zero.
    init.flags = 0;
    st.api = nullptr;
    st.initResult = ctlInit(&init, &st.api);
  }

  if (st.initResult != CTL_RESULT_SUCCESS || !st.api) {
    st.api = nullptr;
    return false;
  }

  uint32_t count = 0;
  if (ctlEnumerateDevices(st.api, &count, nullptr) != CTL_RESULT_SUCCESS || count == 0) {
    // Keep init successful but with no device entries.
    return true;
  }

  std::vector<ctl_device_adapter_handle_t> handles(count);
  if (ctlEnumerateDevices(st.api, &count, handles.data()) != CTL_RESULT_SUCCESS || count == 0) {
    return true;
  }
  handles.resize(count);

  st.devices.clear();
  st.devices.reserve(handles.size());
  for (auto h : handles) {
    IgclDeviceInfo di;
    di.handle = h;
    di.props = {};
    di.props.Size = sizeof(di.props);
    di.props.Version = 2;
    if (ctlGetDeviceProperties(h, &di.props) == CTL_RESULT_SUCCESS) {
      st.devices.push_back(di);
    }
  }

  return true;
}

static std::optional<ctl_device_adapter_handle_t> findDeviceLocked(const IgclState& st, const IntelAdapterPciIds& ids) {
  if (ids.vendorId != 0x8086 || ids.deviceId == 0) return std::nullopt;

  ctl_device_adapter_handle_t best = nullptr;
  for (const auto& d : st.devices) {
    if (d.props.pci_vendor_id != ids.vendorId) continue;
    if (d.props.pci_device_id != ids.deviceId) continue;
    if (ids.subSysId != 0 && d.props.pci_subsys_id != 0 && d.props.pci_subsys_id != ids.subSysId) continue;
    if (ids.revisionId != 0 && d.props.rev_id != 0 && d.props.rev_id != ids.revisionId) continue;
    best = d.handle;
    break;
  }

  if (best) return std::optional<ctl_device_adapter_handle_t>(best);

  // Fallback: if we can't match PCI IDs, but there is exactly one Intel device,
  // use it. This avoids dropping telemetry on single-GPU systems with mismatched IDs.
  ctl_device_adapter_handle_t firstIntel = nullptr;
  for (const auto& d : st.devices) {
    if (d.props.pci_vendor_id == 0x8086) {
      firstIntel = d.handle;
      break;
    }
  }
  if (!firstIntel) return std::nullopt;
  return std::optional<ctl_device_adapter_handle_t>(firstIntel);
}

static std::optional<double> readTemperatureForDevice(ctl_device_adapter_handle_t h) {
  uint32_t count = 0;
  if (ctlEnumTemperatureSensors(h, &count, nullptr) != CTL_RESULT_SUCCESS || count == 0) return std::nullopt;
  std::vector<ctl_temp_handle_t> temps(count);
  if (ctlEnumTemperatureSensors(h, &count, temps.data()) != CTL_RESULT_SUCCESS || count == 0) return std::nullopt;
  double maxTemp = -1.0;
  for (uint32_t i = 0; i < count; ++i) {
    double t = 0.0;
    if (ctlTemperatureGetState(temps[i], &t) == CTL_RESULT_SUCCESS) {
      if (t > maxTemp) maxTemp = t;
    }
  }
  if (maxTemp < 0.0) return std::nullopt;
  return maxTemp;
}

static std::optional<double> readFrequencyForDomain(ctl_device_adapter_handle_t h, ctl_freq_domain_t domain) {
  uint32_t count = 0;
  if (ctlEnumFrequencyDomains(h, &count, nullptr) != CTL_RESULT_SUCCESS || count == 0) return std::nullopt;
  std::vector<ctl_freq_handle_t> domains(count);
  if (ctlEnumFrequencyDomains(h, &count, domains.data()) != CTL_RESULT_SUCCESS || count == 0) return std::nullopt;

  for (uint32_t i = 0; i < count; ++i) {
    ctl_freq_properties_t props{};
    props.Size = sizeof(props);
    props.Version = 0;
    if (ctlFrequencyGetProperties(domains[i], &props) != CTL_RESULT_SUCCESS) continue;
    if (props.type != domain) continue;

    ctl_freq_state_t state{};
    state.Size = sizeof(state);
    state.Version = 0;
    if (ctlFrequencyGetState(domains[i], &state) != CTL_RESULT_SUCCESS) continue;
    if (state.actual >= 0.0) return state.actual;
    if (state.request >= 0.0) return state.request;
  }

  return std::nullopt;
}

struct IgclPciLinkInfo {
  int width = -1;
  int gen = -1;
};

static std::optional<IgclPciLinkInfo> readPciLinkForDevice(ctl_device_adapter_handle_t h) {
  ctl_pci_state_t state{};
  state.Size = sizeof(state);
  state.Version = 0;
  if (ctlPciGetState(h, &state) != CTL_RESULT_SUCCESS) return std::nullopt;

  IgclPciLinkInfo out;
  if (state.speed.width > 0) out.width = state.speed.width;
  if (state.speed.gen > 0) out.gen = state.speed.gen;

  if (out.width > 0 || out.gen > 0) return out;
  return std::nullopt;
}

#endif  // AI_Z_HAS_IGCL_HEADERS

}  // namespace

IgclAvailability igclAvailability() {
  IgclAvailability out;
#if defined(AI_Z_HAS_IGCL_HEADERS) && (AI_Z_HAS_IGCL_HEADERS == 1)
  auto& st = igclState();
  {
    std::lock_guard<std::mutex> lock(st.mu);
    out.available = ensureIgclInitLocked(st) && (st.initResult == CTL_RESULT_SUCCESS);
  }
  out.backend = out.available ? std::string("IGCL") : std::string();
  out.dll = out.available ? std::string("ControlLib.dll") : std::string();
#else
  out.available = false;
#endif
  return out;
}

IgclStatus igclStatus() {
#if defined(AI_Z_HAS_IGCL_HEADERS) && (AI_Z_HAS_IGCL_HEADERS == 1)
  auto& st = igclState();
  std::lock_guard<std::mutex> lock(st.mu);
  (void)ensureIgclInitLocked(st);
  if (st.initResult == CTL_RESULT_SUCCESS) return IgclStatus::Available;
  if (st.initResult == CTL_RESULT_ERROR_LOAD) return IgclStatus::MissingDll;
  return IgclStatus::Unusable;
#else
  return IgclStatus::MissingDll;
#endif
}

std::string igclDiagnostics() {
  const auto st = igclStatus();
  if (st == IgclStatus::Available) return std::string("IGCL diagnostics (Windows)\n- status: available\n");
  if (st == IgclStatus::MissingDll) return std::string("IGCL diagnostics (Windows)\n- status: missing dll\n");
  return std::string("IGCL diagnostics (Windows)\n- status: unusable\n");
}

std::string igclDiagnosticsDetailed() {
#if defined(AI_Z_HAS_IGCL_HEADERS) && (AI_Z_HAS_IGCL_HEADERS == 1)
  std::ostringstream oss;
  oss << "IGCL diagnostics (Windows)\n";

  auto& st = igclState();
  std::lock_guard<std::mutex> lock(st.mu);
  if (!ensureIgclInitLocked(st) || st.initResult != CTL_RESULT_SUCCESS) {
    oss << "- status: unavailable\n";
    return oss.str();
  }

  oss << "- status: available\n";
  oss << "- devices: " << st.devices.size() << "\n";

  for (std::size_t i = 0; i < st.devices.size(); ++i) {
    const auto& d = st.devices[i];
    oss << "  device " << i << ": " << d.props.name << "\n";
    oss << "    pci: " << std::hex << "0x" << d.props.pci_vendor_id
        << ":0x" << d.props.pci_device_id
        << " subsys 0x" << d.props.pci_subsys_id
        << " rev 0x" << d.props.rev_id << std::dec << "\n";

    ctl_power_telemetry_t p{};
    p.Size = sizeof(p);
    p.Version = 1;
    const bool powerOk = (ctlPowerTelemetryGet(d.handle, &p) == CTL_RESULT_SUCCESS);
    oss << "    power telemetry: " << (powerOk ? "ok" : "failed") << "\n";
    if (powerOk) {
      oss << "      temp: " << formatTelemetryItem(p.gpuCurrentTemperature) << "\n";
      oss << "      clock: " << formatTelemetryItem(p.gpuCurrentClockFrequency) << "\n";
      oss << "      energy: " << formatTelemetryItem(p.gpuEnergyCounter) << "\n";
      oss << "      activity: " << formatTelemetryItem(p.globalActivityCounter) << "\n";
      oss << "      timestamp: " << formatTelemetryItem(p.timeStamp) << "\n";
      oss << "      vram_temp: " << formatTelemetryItem(p.vramCurrentTemperature) << "\n";
      oss << "      vram_read_bw: " << formatTelemetryItem(p.vramReadBandwidth) << "\n";
      oss << "      vram_write_bw: " << formatTelemetryItem(p.vramWriteBandwidth) << "\n";
      for (int f = 0; f < CTL_FAN_COUNT; ++f) {
        if (p.fanSpeed[f].bSupported) {
          oss << "      fan[" << f << "]: " << formatTelemetryItem(p.fanSpeed[f]) << "\n";
        }
      }
      // Throttle/limiting flags.
      oss << "      limits: power=" << (p.gpuPowerLimited ? "yes" : "no")
          << " temp=" << (p.gpuTemperatureLimited ? "yes" : "no")
          << " current=" << (p.gpuCurrentLimited ? "yes" : "no")
          << " voltage=" << (p.gpuVoltageLimited ? "yes" : "no")
          << " util=" << (p.gpuUtilizationLimited ? "yes" : "no") << "\n";
    }

    // PCIe link info.
    ctl_pci_state_t pciState{};
    pciState.Size = sizeof(pciState);
    pciState.Version = 0;
    const auto pciResult = ctlPciGetState(d.handle, &pciState);
    if (pciResult == CTL_RESULT_SUCCESS) {
      oss << "    pcie: width=" << pciState.speed.width << " gen=" << pciState.speed.gen << "\n";
    } else {
      oss << "    pcie: failed (0x" << std::hex << static_cast<int>(pciResult) << std::dec << ")\n";
    }

    const auto temp = readTemperatureForDevice(d.handle);
    oss << "    temp sensors: " << (temp ? "ok" : "n/a") << "\n";

    const auto gpuFreq = readFrequencyForDomain(d.handle, CTL_FREQ_DOMAIN_GPU);
    const auto memFreq = readFrequencyForDomain(d.handle, CTL_FREQ_DOMAIN_MEMORY);
    oss << "    freq GPU: " << (gpuFreq ? "ok" : "n/a") << "\n";
    oss << "    freq MEM: " << (memFreq ? "ok" : "n/a") << "\n";
  }

  return oss.str();
#else
  return std::string("IGCL diagnostics\n- status: unavailable (no IGCL headers)\n");
#endif
}

std::optional<IgclGpuTelemetry> readIgclTelemetryForDxgi(const std::optional<IntelAdapterLuid>&) {
  // Deprecated shim: prefer PCI matching via readIgclTelemetryForPciIds().
  return std::nullopt;
}

std::optional<IgclGpuTelemetry> readIgclTelemetryForPciIds(const IntelAdapterPciIds& ids) {
#if defined(AI_Z_HAS_IGCL_HEADERS) && (AI_Z_HAS_IGCL_HEADERS == 1)
  auto& st = igclState();
  std::lock_guard<std::mutex> lock(st.mu);
  if (!ensureIgclInitLocked(st)) return std::nullopt;
  if (st.initResult != CTL_RESULT_SUCCESS) return std::nullopt;

  const auto dev = findDeviceLocked(st, ids);
  if (!dev) return std::nullopt;

  ctl_power_telemetry_t p{};
  p.Size = sizeof(p);
  p.Version = 1;  // Version 1 supports more telemetry fields (fan speed, VR temps, etc.)
  const bool powerOk = (ctlPowerTelemetryGet(*dev, &p) == CTL_RESULT_SUCCESS);

  IgclGpuTelemetry out;

  std::optional<double> ts;
  std::optional<double> energy;
  std::optional<double> globalAct;
  std::optional<double> mediaAct;
  std::optional<double> vramReadBytes;
  std::optional<double> vramWriteBytes;

  if (powerOk) {
    const auto temp = toDouble(p.gpuCurrentTemperature);
    if (temp) out.tempC = *temp;

    const auto clk = toDouble(p.gpuCurrentClockFrequency);
    if (clk) out.gpuClockMHz = static_cast<unsigned int>(std::max(0.0, *clk));

    ts = toDouble(p.timeStamp);
    energy = toDouble(p.gpuEnergyCounter);
    globalAct = toDouble(p.globalActivityCounter);
    mediaAct = toDouble(p.mediaActivityCounter);

    // Read VRAM temperature.
    const auto vramTemp = toDouble(p.vramCurrentTemperature);
    if (vramTemp && *vramTemp > 0.0) out.vramTempC = *vramTemp;

    // Read fan speed (first supported fan).
    for (int i = 0; i < CTL_FAN_COUNT; ++i) {
      if (p.fanSpeed[i].bSupported) {
        const auto fan = toDouble(p.fanSpeed[i]);
        if (fan && *fan >= 0.0) {
          out.fanSpeedRpm = *fan;
          break;
        }
      }
    }

    // Read VRAM bandwidth counters (for calculating MB/s).
    // Use the instantaneous bandwidth fields if available (version > 0).
    const auto vramReadBw = toDouble(p.vramReadBandwidth);
    if (vramReadBw && *vramReadBw >= 0.0) {
      out.vramReadBandwidthMBps = *vramReadBw;
    }
    const auto vramWriteBw = toDouble(p.vramWriteBandwidth);
    if (vramWriteBw && *vramWriteBw >= 0.0) {
      out.vramWriteBandwidthMBps = *vramWriteBw;
    }

    // Read throttle state from limiting flags.
    // Priority: power > thermal > current > voltage > utilization (idle)
    if (p.gpuPowerLimited) {
      out.throttleState = "PWR";
    } else if (p.gpuTemperatureLimited) {
      out.throttleState = "TMP";
    } else if (p.gpuCurrentLimited) {
      out.throttleState = "CUR";
    } else if (p.gpuVoltageLimited) {
      out.throttleState = "VLT";
    } else if (p.gpuUtilizationLimited) {
      out.throttleState = "IDLE";
    }

    // Fallback: calculate from counters if instantaneous values unavailable.
    if (!out.vramReadBandwidthMBps || !out.vramWriteBandwidthMBps) {
      vramReadBytes = toDouble(p.vramReadBandwidthCounter);
      vramWriteBytes = toDouble(p.vramWriteBandwidthCounter);
    }

    IgclSample& s = st.samples[*dev];
    if (ts && energy && s.lastTimestampS >= 0.0 && s.lastGpuEnergyJ >= 0.0) {
      const double dt = *ts - s.lastTimestampS;
      const double de = *energy - s.lastGpuEnergyJ;
      if (dt > 0.0 && de >= 0.0) {
        out.powerW = de / dt;
      }
    }

    if (ts && globalAct && s.lastTimestampS >= 0.0 && s.lastGlobalActivityS >= 0.0) {
      const double dt = *ts - s.lastTimestampS;
      const double da = *globalAct - s.lastGlobalActivityS;
      if (dt > 0.0 && da >= 0.0) {
        out.gpuUtilPct = std::clamp((da / dt) * 100.0, 0.0, 100.0);
      }
    }

    // Calculate VRAM bandwidth from counters if instantaneous values unavailable.
    if (ts && s.lastTimestampS >= 0.0) {
      const double dt = *ts - s.lastTimestampS;
      if (dt > 0.0) {
        if (!out.vramReadBandwidthMBps && vramReadBytes && s.lastVramReadBandwidthBytes >= 0.0) {
          const double db = *vramReadBytes - s.lastVramReadBandwidthBytes;
          if (db >= 0.0) {
            out.vramReadBandwidthMBps = (db / dt) / (1024.0 * 1024.0);
          }
        }
        if (!out.vramWriteBandwidthMBps && vramWriteBytes && s.lastVramWriteBandwidthBytes >= 0.0) {
          const double db = *vramWriteBytes - s.lastVramWriteBandwidthBytes;
          if (db >= 0.0) {
            out.vramWriteBandwidthMBps = (db / dt) / (1024.0 * 1024.0);
          }
        }
      }
    }

    // Update sample state.
    if (ts) s.lastTimestampS = *ts;
    if (energy) s.lastGpuEnergyJ = *energy;
    if (globalAct) s.lastGlobalActivityS = *globalAct;
    if (mediaAct) s.lastMediaActivityS = *mediaAct;
    if (vramReadBytes) s.lastVramReadBandwidthBytes = *vramReadBytes;
    if (vramWriteBytes) s.lastVramWriteBandwidthBytes = *vramWriteBytes;
  }

  if (!out.tempC) {
    if (const auto t = readTemperatureForDevice(*dev)) out.tempC = *t;
  }
  if (!out.gpuClockMHz) {
    if (const auto f = readFrequencyForDomain(*dev, CTL_FREQ_DOMAIN_GPU)) {
      if (*f >= 0.0) out.gpuClockMHz = static_cast<unsigned int>(*f);
    }
  }
  if (!out.memClockMHz) {
    if (const auto f = readFrequencyForDomain(*dev, CTL_FREQ_DOMAIN_MEMORY)) {
      if (*f >= 0.0) out.memClockMHz = static_cast<unsigned int>(*f);
    }
  }

  // Read PCIe link info.
  if (const auto pci = readPciLinkForDevice(*dev)) {
    if (pci->width > 0) out.pcieLinkWidth = pci->width;
    if (pci->gen > 0) out.pcieLinkGen = pci->gen;
  }

  if (!out.tempC && !out.powerW && !out.gpuClockMHz && !out.gpuUtilPct) return std::nullopt;
  return out;
#else
  (void)ids;
  return std::nullopt;
#endif
}

}  // namespace aiz

#else

namespace aiz {

IgclAvailability igclAvailability() {
  return {};
}

std::string igclDiagnostics() {
  return std::string("IGCL diagnostics\n- status: unavailable (non-Windows build)\n");
}

IgclStatus igclStatus() {
  return IgclStatus::MissingDll;
}

std::optional<IgclGpuTelemetry> readIgclTelemetryForDxgi(const std::optional<IntelAdapterLuid>&) {
  return std::nullopt;
}

std::optional<IgclGpuTelemetry> readIgclTelemetryForPciIds(const IntelAdapterPciIds&) {
  return std::nullopt;
}

}  // namespace aiz

#endif
