"""
Tests for AI-Z Python bindings.

These tests verify the Python API works correctly. They're designed to pass
on any Linux system, even without GPU hardware.
"""

import pytest
import time


def test_import():
    """Test that the aiz module can be imported."""
    import aiz
    assert hasattr(aiz, '__version__')
    assert hasattr(aiz, 'sample_cpu')
    assert hasattr(aiz, 'sample_ram')
    assert hasattr(aiz, 'sample_gpu')


def test_version():
    """Test version string is valid."""
    import aiz
    version = aiz.__version__
    assert isinstance(version, str)
    # Should be semantic versioning
    parts = version.split('.')
    assert len(parts) >= 2


def test_sample_cpu():
    """Test CPU sampling returns valid data after priming."""
    import aiz
    
    # First call primes the collector
    result1 = aiz.sample_cpu()
    # First call may return None (needs baseline)
    
    time.sleep(0.1)
    
    # Second call should return a value
    result2 = aiz.sample_cpu()
    if result2 is not None:
        assert isinstance(result2, float)
        assert 0.0 <= result2 <= 100.0


def test_sample_cpu_max_core():
    """Test max-core CPU sampling."""
    import aiz
    
    # Prime
    aiz.sample_cpu_max_core()
    time.sleep(0.1)
    
    result = aiz.sample_cpu_max_core()
    if result is not None:
        assert isinstance(result, float)
        assert 0.0 <= result <= 100.0


def test_sample_ram():
    """Test RAM sampling returns valid data."""
    import aiz
    
    ram = aiz.sample_ram()
    assert ram is not None
    
    assert hasattr(ram, 'used_gib')
    assert hasattr(ram, 'total_gib')
    assert hasattr(ram, 'used_pct')
    
    assert isinstance(ram.used_gib, float)
    assert isinstance(ram.total_gib, float)
    assert isinstance(ram.used_pct, float)
    
    assert ram.total_gib > 0
    assert ram.used_gib >= 0
    assert ram.used_gib <= ram.total_gib
    assert 0.0 <= ram.used_pct <= 100.0


def test_sample_disk():
    """Test disk bandwidth sampling."""
    import aiz
    
    # Prime
    aiz.sample_disk_read()
    aiz.sample_disk_write()
    time.sleep(0.1)
    
    read = aiz.sample_disk_read()
    write = aiz.sample_disk_write()
    
    # Values may be None if no disk activity
    if read is not None:
        assert isinstance(read, float)
        assert read >= 0
    
    if write is not None:
        assert isinstance(write, float)
        assert write >= 0


def test_sample_network():
    """Test network bandwidth sampling."""
    import aiz
    
    # Prime
    aiz.sample_network_rx()
    aiz.sample_network_tx()
    time.sleep(0.1)
    
    rx = aiz.sample_network_rx()
    tx = aiz.sample_network_tx()
    
    if rx is not None:
        assert isinstance(rx, float)
        assert rx >= 0
    
    if tx is not None:
        assert isinstance(tx, float)
        assert tx >= 0


def test_gpu_count():
    """Test GPU count function."""
    import aiz
    
    count = aiz.gpu_count()
    assert isinstance(count, int)
    assert count >= 0


def test_sample_all_gpus():
    """Test sampling all GPUs returns a list."""
    import aiz
    
    gpus = aiz.sample_all_gpus()
    assert isinstance(gpus, list)
    
    for gpu in gpus:
        assert hasattr(gpu, 'index')
        assert hasattr(gpu, 'name')
        assert hasattr(gpu, 'vendor')
        assert hasattr(gpu, 'source')


def test_enumerate_gpus():
    """Test GPU enumeration."""
    import aiz
    
    gpus = aiz.enumerate_gpus()
    assert isinstance(gpus, list)
    
    for gpu in gpus:
        assert hasattr(gpu, 'index')
        assert hasattr(gpu, 'drm_card')
        assert hasattr(gpu, 'driver')
        assert hasattr(gpu, 'vendor')


def test_probe_hardware():
    """Test hardware probing returns valid info."""
    import aiz
    
    hw = aiz.probe_hardware()
    
    assert hasattr(hw, 'os_pretty')
    assert hasattr(hw, 'kernel_version')
    assert hasattr(hw, 'cpu_name')
    assert hasattr(hw, 'ram_summary')
    
    # These should be non-empty strings on any Linux system
    assert isinstance(hw.os_pretty, str)
    assert isinstance(hw.cpu_name, str)
    assert len(hw.cpu_name) > 0
    
    # to_lines should return a list of strings
    lines = hw.to_lines()
    assert isinstance(lines, list)


def test_gpu_vendor_enum():
    """Test GpuVendor enum values."""
    import aiz
    
    assert hasattr(aiz, 'GpuVendor')
    assert hasattr(aiz.GpuVendor, 'Unknown')
    assert hasattr(aiz.GpuVendor, 'Nvidia')
    assert hasattr(aiz.GpuVendor, 'Amd')
    assert hasattr(aiz.GpuVendor, 'Intel')


def test_metrics_class():
    """Test the convenience Metrics class."""
    import aiz
    
    m = aiz.Metrics()
    assert hasattr(m, 'prime')
    assert hasattr(m, 'sample')
    
    m.prime()
    time.sleep(0.2)
    
    snapshot = m.sample()
    assert isinstance(snapshot, dict)
    assert 'cpu_pct' in snapshot
    assert 'ram' in snapshot
    assert 'gpus' in snapshot
    assert 'disk_read_mbps' in snapshot
    assert 'network_rx_mbps' in snapshot


def test_nvml_functions():
    """Test NVML-specific functions don't crash (may return None)."""
    import aiz
    
    # These may return None if no NVIDIA GPU or NVML unavailable
    count = aiz.nvml_gpu_count()
    assert count is None or isinstance(count, int)
    
    if count and count > 0:
        telemetry = aiz.nvml_read_telemetry(0)
        name = aiz.nvml_read_gpu_name(0)
        # Just verify they don't crash


def test_rocm_functions():
    """Test ROCm-specific functions don't crash (may return None)."""
    import aiz
    
    count = aiz.rocm_gpu_count()
    assert count is None or isinstance(count, int)


if __name__ == "__main__":
    pytest.main([__file__, "-v"])
