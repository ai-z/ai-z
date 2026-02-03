# AI-Z Python Library

Hardware metrics library for CPU, RAM, GPU telemetry on Linux.

This Python package provides bindings to the AI-Z C++ metrics library, enabling real-time monitoring of:

- **CPU**: Usage percentage, per-core max usage
- **RAM**: Used/total memory in GiB, percentage
- **GPU**: Utilization, VRAM, power, temperature (NVIDIA via NVML, AMD via ROCm SMI, Intel via sysfs)
- **Disk**: Read/write bandwidth in MB/s
- **Network**: RX/TX bandwidth in MB/s
- **Hardware Info**: CPU model, GPU model, driver versions, etc.

## Installation

```bash
pip install ai-z
```

## Quick Start

```python
import aiz
import time

# Probe hardware info
hw = aiz.probe_hardware()
print(f"CPU: {hw.cpu_name}")
print(f"GPU: {hw.gpu_name}")

# Sample metrics (first call establishes baseline for rate-based metrics)
aiz.sample_cpu()
aiz.sample_disk_read()
time.sleep(0.5)

# Now get actual values
cpu = aiz.sample_cpu()
ram = aiz.sample_ram()
print(f"CPU: {cpu:.1f}%")
print(f"RAM: {ram.used_gib:.1f}/{ram.total_gib:.1f} GiB ({ram.used_pct:.1f}%)")

# GPU metrics
for gpu in aiz.sample_all_gpus():
    print(f"GPU {gpu.index}: {gpu.name}")
    if gpu.util_pct is not None:
        print(f"  Utilization: {gpu.util_pct:.1f}%")
    if gpu.temp_c is not None:
        print(f"  Temperature: {gpu.temp_c:.0f}°C")
```

## API Reference

### CPU Metrics

- `aiz.sample_cpu() -> float | None` - Sample overall CPU usage (0-100%)
- `aiz.sample_cpu_max_core() -> float | None` - Sample max single-core usage

### RAM Metrics

- `aiz.sample_ram() -> RamUsage | None` - Sample RAM usage
  - `.used_gib: float` - Used RAM in GiB
  - `.total_gib: float` - Total RAM in GiB
  - `.used_pct: float` - Usage percentage

### GPU Metrics

- `aiz.gpu_count() -> int` - Number of detected GPUs
- `aiz.sample_gpu(index: int) -> GpuTelemetry | None` - Sample specific GPU
- `aiz.sample_all_gpus() -> list[GpuTelemetry]` - Sample all GPUs
- `aiz.enumerate_gpus() -> list[LinuxGpuDevice]` - List GPU devices

#### GpuTelemetry attributes:
- `.index: int` - GPU index
- `.name: str` - GPU name
- `.vendor: GpuVendor` - Nvidia/Amd/Intel/Unknown
- `.util_pct: float | None` - GPU utilization (0-100%)
- `.vram_used_gib: float | None` - VRAM used in GiB
- `.vram_total_gib: float | None` - Total VRAM in GiB
- `.power_watts: float | None` - Power draw in watts
- `.temp_c: float | None` - Temperature in Celsius
- `.source: str` - Data source ("nvml", "rocm-smi", "sysfs")

### Disk Metrics

- `aiz.sample_disk_read() -> float | None` - Disk read bandwidth (MB/s)
- `aiz.sample_disk_write() -> float | None` - Disk write bandwidth (MB/s)

### Network Metrics

- `aiz.sample_network_rx() -> float | None` - Network receive bandwidth (MB/s)
- `aiz.sample_network_tx() -> float | None` - Network transmit bandwidth (MB/s)

### Hardware Info

- `aiz.probe_hardware() -> HardwareInfo` - Probe system hardware

## Requirements

- Linux (x86_64)
- Python 3.9-3.12
- For NVIDIA GPUs: NVIDIA driver with NVML (libnvidia-ml.so)
- For AMD GPUs: ROCm SMI library (optional, falls back to sysfs)

## License

MIT License - see LICENSE file for details.
