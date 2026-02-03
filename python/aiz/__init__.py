"""
AI-Z Hardware Metrics Library

Provides real-time hardware telemetry for CPU, RAM, GPU, disk, and network on Linux.
"""

from __future__ import annotations

from typing import Optional, List

# Import the native extension module
from ._aiz_core import (
    # Structs
    Sample,
    RamUsage,
    NvmlTelemetry,
    LinuxGpuTelemetry,
    LinuxGpuDevice,
    GpuTelemetry,
    GpuVendor,
    HardwareInfo,
    # CPU functions
    sample_cpu,
    sample_cpu_max_core,
    # RAM functions
    sample_ram,
    # Disk functions
    sample_disk_read,
    sample_disk_write,
    # Network functions
    sample_network_rx,
    sample_network_tx,
    # GPU functions
    gpu_count,
    enumerate_gpus,
    sample_gpu,
    sample_all_gpus,
    # NVML-specific
    nvml_gpu_count,
    nvml_read_telemetry,
    nvml_read_gpu_name,
    # ROCm-specific
    rocm_gpu_count,
    # Hardware info
    probe_hardware,
)

__version__ = "0.1.64"

__all__ = [
    # Version
    "__version__",
    # Structs
    "Sample",
    "RamUsage",
    "NvmlTelemetry",
    "LinuxGpuTelemetry",
    "LinuxGpuDevice",
    "GpuTelemetry",
    "GpuVendor",
    "HardwareInfo",
    # CPU
    "sample_cpu",
    "sample_cpu_max_core",
    # RAM
    "sample_ram",
    # Disk
    "sample_disk_read",
    "sample_disk_write",
    # Network
    "sample_network_rx",
    "sample_network_tx",
    # GPU
    "gpu_count",
    "enumerate_gpus",
    "sample_gpu",
    "sample_all_gpus",
    # NVML
    "nvml_gpu_count",
    "nvml_read_telemetry",
    "nvml_read_gpu_name",
    # ROCm
    "rocm_gpu_count",
    # Hardware
    "probe_hardware",
]


class Metrics:
    """
    Convenience class for sampling all metrics at once.
    
    Example:
        >>> m = aiz.Metrics()
        >>> m.prime()  # Establish baselines
        >>> time.sleep(0.5)
        >>> snapshot = m.sample()
        >>> print(snapshot)
    """
    
    def __init__(self):
        self._primed = False
    
    def prime(self) -> None:
        """
        Prime rate-based collectors (CPU, disk, network) by taking an initial sample.
        Call this once, wait ~500ms, then call sample() for accurate values.
        """
        sample_cpu()
        sample_cpu_max_core()
        sample_disk_read()
        sample_disk_write()
        sample_network_rx()
        sample_network_tx()
        self._primed = True
    
    def sample(self) -> dict:
        """
        Sample all metrics and return as a dictionary.
        
        Returns:
            dict with keys: cpu_pct, cpu_max_core_pct, ram, gpus, disk_read_mbps,
            disk_write_mbps, network_rx_mbps, network_tx_mbps
        """
        return {
            "cpu_pct": sample_cpu(),
            "cpu_max_core_pct": sample_cpu_max_core(),
            "ram": sample_ram(),
            "gpus": sample_all_gpus(),
            "disk_read_mbps": sample_disk_read(),
            "disk_write_mbps": sample_disk_write(),
            "network_rx_mbps": sample_network_rx(),
            "network_tx_mbps": sample_network_tx(),
        }
