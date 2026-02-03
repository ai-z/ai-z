#include <aiz/platform/metrics/disk.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>

namespace aiz::platform {

std::optional<DiskCounters> readDiskCounters(const std::string& deviceFilter) {
  DiskCounters result{};

  for (int i = 0; i < 16; ++i) {
    std::string path = "\\\\.\\PhysicalDrive" + std::to_string(i);
    if (!deviceFilter.empty()) {
      if (path.rfind(deviceFilter, 0) != 0 && std::to_string(i).rfind(deviceFilter, 0) != 0) {
        continue;
      }
    }

    HANDLE hDevice = CreateFileA(
        path.c_str(),
        0,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        0,
        nullptr);

    if (hDevice == INVALID_HANDLE_VALUE) continue;

    DISK_PERFORMANCE perf{};
    DWORD bytesReturned = 0;
    if (DeviceIoControl(
            hDevice,
            IOCTL_DISK_PERFORMANCE,
            nullptr, 0,
            &perf, sizeof(perf),
            &bytesReturned,
            nullptr)) {
      result.readBytes += static_cast<std::uint64_t>(perf.BytesRead.QuadPart);
      result.writeBytes += static_cast<std::uint64_t>(perf.BytesWritten.QuadPart);
    }

    CloseHandle(hDevice);
  }

  return result;
}

}  // namespace aiz::platform
