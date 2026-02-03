#include <aiz/platform/process.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>

#include <string>
#include <vector>

namespace aiz::platform {
namespace {

static std::uint64_t fileTimeToU64(const FILETIME& ft) {
  return (static_cast<std::uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

static bool getProcessTimesU64(HANDLE hProcess, std::uint64_t& out) {
  FILETIME createTime{}, exitTime{}, kernelTime{}, userTime{};
  if (!GetProcessTimes(hProcess, &createTime, &exitTime, &kernelTime, &userTime)) return false;
  out = fileTimeToU64(kernelTime) + fileTimeToU64(userTime);
  return true;
}

static bool getProcessMemoryBytes(HANDLE hProcess, std::uint64_t& out) {
  PROCESS_MEMORY_COUNTERS pmc{};
  pmc.cb = sizeof(pmc);
  if (!GetProcessMemoryInfo(hProcess, &pmc, sizeof(pmc))) return false;
  out = static_cast<std::uint64_t>(pmc.WorkingSetSize);
  return true;
}

static std::string getProcessImagePath(HANDLE hProcess) {
  char buffer[MAX_PATH] = {};
  DWORD size = static_cast<DWORD>(sizeof(buffer));
  if (QueryFullProcessImageNameA(hProcess, 0, buffer, &size)) {
    return std::string(buffer, size);
  }
  return {};
}

static bool getTokenUserSid(HANDLE token, std::vector<char>& sidOut) {
  DWORD len = 0;
  GetTokenInformation(token, TokenUser, nullptr, 0, &len);
  if (len == 0) return false;
  sidOut.resize(len);
  if (!GetTokenInformation(token, TokenUser, sidOut.data(), len, &len)) return false;
  return true;
}

static bool getCurrentUserSid(std::vector<char>& sidOut) {
  HANDLE token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
  const bool ok = getTokenUserSid(token, sidOut);
  CloseHandle(token);
  return ok;
}

static bool sidEqual(const std::vector<char>& a, const std::vector<char>& b) {
  const auto* ta = reinterpret_cast<const TOKEN_USER*>(a.data());
  const auto* tb = reinterpret_cast<const TOKEN_USER*>(b.data());
  if (!ta || !tb) return false;
  return EqualSid(ta->User.Sid, tb->User.Sid) != FALSE;
}

}  // namespace

std::vector<ProcessInfo> enumerateUserProcesses() {
  std::vector<ProcessInfo> out;

  std::vector<char> currentSid;
  const bool haveCurrentSid = getCurrentUserSid(currentSid);

  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap == INVALID_HANDLE_VALUE) return out;

  PROCESSENTRY32 pe{};
  pe.dwSize = sizeof(pe);
  if (!Process32First(snap, &pe)) {
    CloseHandle(snap);
    return out;
  }

  do {
    const ProcessId pid = static_cast<ProcessId>(pe.th32ProcessID);
    if (pid == 0) continue;

    if (haveCurrentSid && !isUserProcess(pid)) continue;

    HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!hProcess) continue;

    std::uint64_t cpuJiffies = 0;
    std::uint64_t memBytes = 0;
    getProcessTimesU64(hProcess, cpuJiffies);
    getProcessMemoryBytes(hProcess, memBytes);

    ProcessInfo info;
    info.pid = pid;
    info.name = pe.szExeFile;
    info.cmdline = getProcessImagePath(hProcess);
    info.cpuJiffies = cpuJiffies;
    info.memoryBytes = memBytes;
    out.push_back(std::move(info));

    CloseHandle(hProcess);
  } while (Process32Next(snap, &pe));

  CloseHandle(snap);
  return out;
}

std::optional<std::uint64_t> readTotalCpuJiffies() {
  FILETIME idleTime{}, kernelTime{}, userTime{};
  if (!GetSystemTimes(&idleTime, &kernelTime, &userTime)) return std::nullopt;
  const std::uint64_t kernel = fileTimeToU64(kernelTime);
  const std::uint64_t user = fileTimeToU64(userTime);
  return kernel + user;
}

bool isUserProcess(ProcessId pid) {
  std::vector<char> currentSid;
  if (!getCurrentUserSid(currentSid)) return false;

  HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!hProcess) return false;

  HANDLE token = nullptr;
  if (!OpenProcessToken(hProcess, TOKEN_QUERY, &token)) {
    CloseHandle(hProcess);
    return false;
  }

  std::vector<char> procSid;
  const bool ok = getTokenUserSid(token, procSid);

  CloseHandle(token);
  CloseHandle(hProcess);

  if (!ok) return false;
  return sidEqual(currentSid, procSid);
}

}  // namespace aiz::platform
