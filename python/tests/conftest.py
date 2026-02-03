"""Pytest configuration for AI-Z tests."""

import pytest


def pytest_configure(config):
    """Configure pytest markers."""
    config.addinivalue_line(
        "markers", "gpu: marks tests that require GPU hardware"
    )


@pytest.fixture(scope="session")
def has_nvidia_gpu():
    """Check if an NVIDIA GPU with NVML is available."""
    try:
        import aiz
        count = aiz.nvml_gpu_count()
        return count is not None and count > 0
    except Exception:
        return False


@pytest.fixture(scope="session")
def has_amd_gpu():
    """Check if an AMD GPU with ROCm SMI is available."""
    try:
        import aiz
        count = aiz.rocm_gpu_count()
        return count is not None and count > 0
    except Exception:
        return False


@pytest.fixture(scope="session")
def has_any_gpu():
    """Check if any GPU is detected."""
    try:
        import aiz
        return aiz.gpu_count() > 0
    except Exception:
        return False
