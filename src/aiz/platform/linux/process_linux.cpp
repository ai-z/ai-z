#include <aiz/platform/process.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include <dirent.h>
#include <sys/types.h>
#include <unistd.h>

namespace aiz::platform {
namespace {

static bool readTotalJiffies(std::uint64_t& totalOut) {
  std::ifstream in("/proc/stat");
  if (!in.is_open()) return false;

  std::string line;
  if (!std::getline(in, line)) return false;

  std::istringstream iss(line);
  std::string cpu;
  iss >> cpu;
  if (cpu != "cpu") return false;

  std::uint64_t user = 0, nice = 0, system = 0, idle = 0, iowait = 0, irq = 0, softirq = 0, steal = 0;
  iss >> user >> nice >> system >> idle >> iowait >> irq >> softirq >> steal;
  totalOut = user + nice + system + idle + iowait + irq + softirq + steal;
  return totalOut != 0;
}

static bool readProcStatTimes(int pid, std::uint64_t& procJiffies, std::string& nameOut) {
  std::string path = "/proc/" + std::to_string(pid) + "/stat";
  std::ifstream in(path);
  if (!in.is_open()) return false;

  std::string line;
  if (!std::getline(in, line)) return false;

  const std::size_t lparen = line.find('(');
  const std::size_t rparen = line.rfind(')');
  if (lparen == std::string::npos || rparen == std::string::npos || rparen <= lparen) return false;

  nameOut = line.substr(lparen + 1, rparen - lparen - 1);
  if (rparen + 2 >= line.size()) return false;

  std::string rest = line.substr(rparen + 2);
  std::istringstream iss(rest);
  std::string state;
  iss >> state;

  unsigned long long dummy = 0;
  for (int i = 0; i < 10; ++i) {
    if (!(iss >> dummy)) return false;
  }

  unsigned long long utime = 0;
  unsigned long long stime = 0;
  if (!(iss >> utime >> stime)) return false;

  procJiffies = static_cast<std::uint64_t>(utime + stime);
  return true;
}

static bool readProcRssBytes(int pid, std::uint64_t& rssBytesOut) {
  std::string path = "/proc/" + std::to_string(pid) + "/statm";
  std::ifstream in(path);
  if (!in.is_open()) return false;

  std::uint64_t sizePages = 0;
  std::uint64_t residentPages = 0;
  if (!(in >> sizePages >> residentPages)) return false;

  const long pageSize = ::sysconf(_SC_PAGESIZE);
  if (pageSize <= 0) return false;

  rssBytesOut = residentPages * static_cast<std::uint64_t>(pageSize);
  return true;
}

static bool readProcCmdline(int pid, std::string& cmdlineOut) {
  std::string path = "/proc/" + std::to_string(pid) + "/cmdline";
  std::ifstream in(path, std::ios::in | std::ios::binary);
  if (!in.is_open()) return false;

  std::string buf((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  if (buf.empty()) return false;

  for (char& ch : buf) {
    if (ch == '\0') ch = ' ';
  }

  while (!buf.empty() && std::isspace(static_cast<unsigned char>(buf.back()))) buf.pop_back();
  while (!buf.empty() && std::isspace(static_cast<unsigned char>(buf.front()))) buf.erase(buf.begin());

  if (buf.empty()) return false;
  cmdlineOut = std::move(buf);
  return true;
}

static bool readProcUid(int pid, uid_t& uidOut) {
  std::string path = "/proc/" + std::to_string(pid) + "/status";
  std::ifstream in(path);
  if (!in.is_open()) return false;

  std::string line;
  while (std::getline(in, line)) {
    if (line.rfind("Uid:", 0) != 0) continue;
    std::istringstream iss(line.substr(4));
    unsigned int uid = 0;
    if (!(iss >> uid)) return false;
    uidOut = static_cast<uid_t>(uid);
    return true;
  }
  return false;
}

static bool isDigits(const char* s) {
  if (!s || !*s) return false;
  for (const char* p = s; *p; ++p) {
    if (*p < '0' || *p > '9') return false;
  }
  return true;
}

}  // namespace

std::vector<ProcessInfo> enumerateUserProcesses() {
  std::vector<ProcessInfo> out;

  DIR* dir = ::opendir("/proc");
  if (!dir) return out;

  struct dirent* ent = nullptr;
  while ((ent = ::readdir(dir)) != nullptr) {
    if (ent->d_type != DT_DIR && ent->d_type != DT_LNK && ent->d_type != DT_UNKNOWN) continue;
    if (!isDigits(ent->d_name)) continue;
    const int pid = std::atoi(ent->d_name);
    if (pid <= 0) continue;

    if (!isUserProcess(static_cast<ProcessId>(pid))) continue;

    std::uint64_t procJiffies = 0;
    std::string name;
    if (!readProcStatTimes(pid, procJiffies, name)) continue;

    std::uint64_t rssBytes = 0;
    if (!readProcRssBytes(pid, rssBytes)) continue;

    std::string cmdline;
    readProcCmdline(pid, cmdline);

    ProcessInfo info;
    info.pid = static_cast<ProcessId>(pid);
    info.name = std::move(name);
    info.cmdline = std::move(cmdline);
    info.cpuJiffies = procJiffies;
    info.memoryBytes = rssBytes;
    out.push_back(std::move(info));
  }

  ::closedir(dir);
  return out;
}

std::optional<std::uint64_t> readTotalCpuJiffies() {
  std::uint64_t total = 0;
  if (!readTotalJiffies(total)) return std::nullopt;
  return total;
}

bool isUserProcess(ProcessId pid) {
  uid_t uid = 0;
  if (!readProcUid(static_cast<int>(pid), uid)) return false;
  return uid == ::getuid();
}

}  // namespace aiz::platform
