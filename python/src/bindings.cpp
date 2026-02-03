// SPDX-License-Identifier: MIT
// Python bindings for AI-Z metrics library

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

// pybind11 moved/removed optional caster headers across major versions.
// Prefer the newer location when available.
#if defined(__has_include)
    #if __has_include(<pybind11/stl/optional.h>)
        #include <pybind11/stl/optional.h>
    #elif __has_include(<pybind11/optional.h>)
        #include <pybind11/optional.h>
    #endif
#else
    #include <pybind11/optional.h>
#endif

#include <aiz/metrics/collectors.h>
#include <aiz/metrics/cpu_usage.h>
#include <aiz/metrics/ram_usage.h>
#include <aiz/metrics/disk_bandwidth.h>
#include <aiz/metrics/network_bandwidth.h>
#include <aiz/metrics/nvidia_nvml.h>
#include <aiz/metrics/linux_gpu_sysfs.h>
#include <aiz/metrics/amd_rocm_smi.h>
#include <aiz/metrics/gpu_sampler.h>
#include <aiz/hw/hardware_info.h>

namespace py = pybind11;

// Thread-local collectors for stateful sampling
thread_local aiz::CpuUsageCollector tl_cpuCollector;
thread_local aiz::CpuMaxCoreUsageCollector tl_cpuMaxCoreCollector;
thread_local std::unique_ptr<aiz::DiskBandwidthCollector> tl_diskReadCollector;
thread_local std::unique_ptr<aiz::DiskBandwidthCollector> tl_diskWriteCollector;
thread_local std::unique_ptr<aiz::NetworkBandwidthCollector> tl_netRxCollector;
thread_local std::unique_ptr<aiz::NetworkBandwidthCollector> tl_netTxCollector;

PYBIND11_MODULE(_aiz_core, m) {
    m.doc() = "AI-Z hardware metrics library - Python bindings";

    // --- Sample struct ---
    py::class_<aiz::Sample>(m, "Sample")
        .def(py::init<>())
        .def_readwrite("value", &aiz::Sample::value)
        .def_readwrite("unit", &aiz::Sample::unit)
        .def_readwrite("label", &aiz::Sample::label)
        .def("__repr__", [](const aiz::Sample& s) {
            return "<Sample value=" + std::to_string(s.value) + " unit='" + s.unit + "'>";
        });

    // --- RamUsage struct ---
    py::class_<aiz::RamUsage>(m, "RamUsage")
        .def(py::init<>())
        .def_readwrite("used_gib", &aiz::RamUsage::usedGiB)
        .def_readwrite("total_gib", &aiz::RamUsage::totalGiB)
        .def_readwrite("used_pct", &aiz::RamUsage::usedPct)
        .def("__repr__", [](const aiz::RamUsage& r) {
            return "<RamUsage used=" + std::to_string(r.usedGiB) + " GiB / " +
                   std::to_string(r.totalGiB) + " GiB (" + std::to_string(r.usedPct) + "%)>";
        });

    // --- NvmlTelemetry struct ---
    py::class_<aiz::NvmlTelemetry>(m, "NvmlTelemetry")
        .def(py::init<>())
        .def_readwrite("gpu_util_pct", &aiz::NvmlTelemetry::gpuUtilPct)
        .def_readwrite("mem_util_pct", &aiz::NvmlTelemetry::memUtilPct)
        .def_readwrite("mem_used_gib", &aiz::NvmlTelemetry::memUsedGiB)
        .def_readwrite("mem_total_gib", &aiz::NvmlTelemetry::memTotalGiB)
        .def_readwrite("power_watts", &aiz::NvmlTelemetry::powerWatts)
        .def_readwrite("temp_c", &aiz::NvmlTelemetry::tempC)
        .def_readwrite("pstate", &aiz::NvmlTelemetry::pstate)
        .def_readwrite("encoder_util_pct", &aiz::NvmlTelemetry::encoderUtilPct)
        .def_readwrite("decoder_util_pct", &aiz::NvmlTelemetry::decoderUtilPct)
        .def_readwrite("gpu_clock_mhz", &aiz::NvmlTelemetry::gpuClockMHz)
        .def_readwrite("mem_clock_mhz", &aiz::NvmlTelemetry::memClockMHz)
        .def("__repr__", [](const aiz::NvmlTelemetry& t) {
            return "<NvmlTelemetry gpu=" + std::to_string(t.gpuUtilPct) + "% mem=" +
                   std::to_string(t.memUsedGiB) + "/" + std::to_string(t.memTotalGiB) +
                   " GiB temp=" + std::to_string(t.tempC) + "C>";
        });

    // --- LinuxGpuTelemetry struct ---
    py::class_<aiz::LinuxGpuTelemetry>(m, "LinuxGpuTelemetry")
        .def(py::init<>())
        .def_property_readonly("util_pct", [](const aiz::LinuxGpuTelemetry& t) {
            return t.utilPct;
        })
        .def_property_readonly("vram_used_gib", [](const aiz::LinuxGpuTelemetry& t) {
            return t.vramUsedGiB;
        })
        .def_property_readonly("vram_total_gib", [](const aiz::LinuxGpuTelemetry& t) {
            return t.vramTotalGiB;
        })
        .def_property_readonly("watts", [](const aiz::LinuxGpuTelemetry& t) {
            return t.watts;
        })
        .def_property_readonly("temp_c", [](const aiz::LinuxGpuTelemetry& t) {
            return t.tempC;
        })
        .def_readwrite("pstate", &aiz::LinuxGpuTelemetry::pstate)
        .def_readwrite("source", &aiz::LinuxGpuTelemetry::source)
        .def("__repr__", [](const aiz::LinuxGpuTelemetry& t) {
            std::string s = "<LinuxGpuTelemetry";
            if (t.utilPct) s += " util=" + std::to_string(*t.utilPct) + "%";
            if (t.vramUsedGiB && t.vramTotalGiB)
                s += " vram=" + std::to_string(*t.vramUsedGiB) + "/" + std::to_string(*t.vramTotalGiB) + " GiB";
            if (t.tempC) s += " temp=" + std::to_string(*t.tempC) + "C";
            s += " source='" + t.source + "'>";
            return s;
        });

    // --- GpuVendor enum ---
    py::enum_<aiz::GpuVendor>(m, "GpuVendor")
        .value("Unknown", aiz::GpuVendor::Unknown)
        .value("Nvidia", aiz::GpuVendor::Nvidia)
        .value("Amd", aiz::GpuVendor::Amd)
        .value("Intel", aiz::GpuVendor::Intel)
        .export_values();

    // --- LinuxGpuDevice struct ---
    py::class_<aiz::LinuxGpuDevice>(m, "LinuxGpuDevice")
        .def(py::init<>())
        .def_readwrite("index", &aiz::LinuxGpuDevice::index)
        .def_readwrite("drm_card", &aiz::LinuxGpuDevice::drmCard)
        .def_readwrite("sysfs_device_path", &aiz::LinuxGpuDevice::sysfsDevicePath)
        .def_readwrite("pci_slot_name", &aiz::LinuxGpuDevice::pciSlotName)
        .def_readwrite("vendor", &aiz::LinuxGpuDevice::vendor)
        .def_readwrite("driver", &aiz::LinuxGpuDevice::driver)
        .def("__repr__", [](const aiz::LinuxGpuDevice& d) {
            return "<LinuxGpuDevice index=" + std::to_string(d.index) +
                   " card='" + d.drmCard + "' driver='" + d.driver + "'>";
        });

    // --- GpuTelemetry (unified) struct ---
    py::class_<aiz::GpuTelemetry>(m, "GpuTelemetry")
        .def(py::init<>())
        .def_readwrite("index", &aiz::GpuTelemetry::index)
        .def_readwrite("name", &aiz::GpuTelemetry::name)
        .def_readwrite("vendor", &aiz::GpuTelemetry::vendor)
        .def_property_readonly("util_pct", [](const aiz::GpuTelemetry& t) { return t.utilPct; })
        .def_property_readonly("vram_used_gib", [](const aiz::GpuTelemetry& t) { return t.vramUsedGiB; })
        .def_property_readonly("vram_total_gib", [](const aiz::GpuTelemetry& t) { return t.vramTotalGiB; })
        .def_property_readonly("power_watts", [](const aiz::GpuTelemetry& t) { return t.powerWatts; })
        .def_property_readonly("temp_c", [](const aiz::GpuTelemetry& t) { return t.tempC; })
        .def_readwrite("pstate", &aiz::GpuTelemetry::pstate)
        .def_readwrite("source", &aiz::GpuTelemetry::source)
        .def("__repr__", [](const aiz::GpuTelemetry& t) {
            std::string s = "<GpuTelemetry index=" + std::to_string(t.index) + " name='" + t.name + "'";
            if (t.utilPct) s += " util=" + std::to_string(*t.utilPct) + "%";
            if (t.vramUsedGiB && t.vramTotalGiB)
                s += " vram=" + std::to_string(*t.vramUsedGiB) + "/" + std::to_string(*t.vramTotalGiB) + " GiB";
            if (t.tempC) s += " temp=" + std::to_string(*t.tempC) + "C";
            s += ">";
            return s;
        });

    // --- HardwareInfo struct ---
    py::class_<aiz::HardwareInfo>(m, "HardwareInfo")
        .def(py::init<>())
        .def_readwrite("os_pretty", &aiz::HardwareInfo::osPretty)
        .def_readwrite("kernel_version", &aiz::HardwareInfo::kernelVersion)
        .def_readwrite("cpu_name", &aiz::HardwareInfo::cpuName)
        .def_readwrite("cpu_physical_cores", &aiz::HardwareInfo::cpuPhysicalCores)
        .def_readwrite("cpu_logical_cores", &aiz::HardwareInfo::cpuLogicalCores)
        .def_readwrite("cpu_cache_l1", &aiz::HardwareInfo::cpuCacheL1)
        .def_readwrite("cpu_cache_l2", &aiz::HardwareInfo::cpuCacheL2)
        .def_readwrite("cpu_cache_l3", &aiz::HardwareInfo::cpuCacheL3)
        .def_readwrite("ram_summary", &aiz::HardwareInfo::ramSummary)
        .def_readwrite("gpu_name", &aiz::HardwareInfo::gpuName)
        .def_readwrite("gpu_driver", &aiz::HardwareInfo::gpuDriver)
        .def_readwrite("per_gpu_lines", &aiz::HardwareInfo::perGpuLines)
        .def_readwrite("vram_summary", &aiz::HardwareInfo::vramSummary)
        .def_readwrite("cuda_version", &aiz::HardwareInfo::cudaVersion)
        .def_readwrite("nvml_version", &aiz::HardwareInfo::nvmlVersion)
        .def_readwrite("rocm_version", &aiz::HardwareInfo::rocmVersion)
        .def_readwrite("opencl_version", &aiz::HardwareInfo::openclVersion)
        .def_readwrite("vulkan_version", &aiz::HardwareInfo::vulkanVersion)
        .def_readwrite("npu_summary", &aiz::HardwareInfo::npuSummary)
        .def("to_lines", &aiz::HardwareInfo::toLines)
        .def_static("probe", &aiz::HardwareInfo::probe)
        .def("__repr__", [](const aiz::HardwareInfo& h) {
            return "<HardwareInfo cpu='" + h.cpuName + "' gpu='" + h.gpuName + "'>";
        });

    // --- CPU sampling functions ---
    m.def("sample_cpu", []() -> std::optional<double> {
        auto s = tl_cpuCollector.sample();
        if (s) return s->value;
        return std::nullopt;
    }, "Sample CPU usage percentage (0-100). Returns None on first call (needs baseline).");

    m.def("sample_cpu_max_core", []() -> std::optional<double> {
        auto s = tl_cpuMaxCoreCollector.sample();
        if (s) return s->value;
        return std::nullopt;
    }, "Sample maximum single-core CPU usage percentage (0-100).");

    // --- RAM sampling function ---
    m.def("sample_ram", &aiz::readRamUsage,
        "Sample RAM usage. Returns RamUsage with used_gib, total_gib, used_pct.");

    // --- Disk bandwidth sampling ---
    m.def("sample_disk_read", []() -> std::optional<double> {
        if (!tl_diskReadCollector) {
            tl_diskReadCollector = std::make_unique<aiz::DiskBandwidthCollector>(
                aiz::DiskBandwidthMode::Read);
        }
        auto s = tl_diskReadCollector->sample();
        if (s) return s->value;
        return std::nullopt;
    }, "Sample disk read bandwidth in MB/s.");

    m.def("sample_disk_write", []() -> std::optional<double> {
        if (!tl_diskWriteCollector) {
            tl_diskWriteCollector = std::make_unique<aiz::DiskBandwidthCollector>(
                aiz::DiskBandwidthMode::Write);
        }
        auto s = tl_diskWriteCollector->sample();
        if (s) return s->value;
        return std::nullopt;
    }, "Sample disk write bandwidth in MB/s.");

    // --- Network bandwidth sampling ---
    m.def("sample_network_rx", []() -> std::optional<double> {
        if (!tl_netRxCollector) {
            tl_netRxCollector = std::make_unique<aiz::NetworkBandwidthCollector>(
                aiz::NetworkBandwidthMode::Rx);
        }
        auto s = tl_netRxCollector->sample();
        if (s) return s->value;
        return std::nullopt;
    }, "Sample network receive bandwidth in MB/s.");

    m.def("sample_network_tx", []() -> std::optional<double> {
        if (!tl_netTxCollector) {
            tl_netTxCollector = std::make_unique<aiz::NetworkBandwidthCollector>(
                aiz::NetworkBandwidthMode::Tx);
        }
        auto s = tl_netTxCollector->sample();
        if (s) return s->value;
        return std::nullopt;
    }, "Sample network transmit bandwidth in MB/s.");

    // --- GPU enumeration and sampling ---
    m.def("gpu_count", &aiz::gpuCount,
        "Returns the number of detected GPUs (NVML + Linux sysfs).");

    m.def("enumerate_gpus", &aiz::enumerateLinuxGpus,
        "Enumerate all Linux GPUs via /sys/class/drm.");

    m.def("sample_gpu", &aiz::sampleGpuTelemetry,
        py::arg("index"),
        "Sample telemetry for GPU at given index. Returns GpuTelemetry or None.");

    m.def("sample_all_gpus", &aiz::sampleAllGpuTelemetry,
        "Sample telemetry for all detected GPUs. Returns list of GpuTelemetry.");

    // --- NVML-specific functions ---
    m.def("nvml_gpu_count", &aiz::nvmlGpuCount,
        "Returns the number of NVML-visible GPUs, or None if NVML unavailable.");

    m.def("nvml_read_telemetry", &aiz::readNvmlTelemetryForGpu,
        py::arg("index"),
        "Read NVML telemetry for specific GPU index.");

    m.def("nvml_read_gpu_name", &aiz::readNvmlGpuNameForGpu,
        py::arg("index"),
        "Read GPU name via NVML for specific GPU index.");

    // --- ROCm SMI functions ---
    m.def("rocm_gpu_count", &aiz::rocmSmiGpuCount,
        "Returns the number of ROCm SMI-visible GPUs, or None if unavailable.");

    // --- Hardware info ---
    m.def("probe_hardware", &aiz::HardwareInfo::probe,
        "Probe system hardware and return HardwareInfo.");
}
