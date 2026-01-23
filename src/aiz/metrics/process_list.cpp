#include <aiz/metrics/process_list.h>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <dirent.h>
#include <sys/types.h>
#include <unistd.h>

namespace aiz {
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

  // Parse "pid (comm) state ..." with comm possibly containing spaces.
  const std::size_t lparen = line.find('(');
  const std::size_t rparen = line.rfind(')');
  if (lparen == std::string::npos || rparen == std::string::npos || rparen <= lparen) return false;

  nameOut = line.substr(lparen + 1, rparen - lparen - 1);
  if (rparen + 2 >= line.size()) return false;

  std::string rest = line.substr(rparen + 2);
  std::istringstream iss(rest);
  std::string state;
  iss >> state;  // field 3

  // Skip fields 4..13 (10 fields) to reach utime/stime (14/15).
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

static bool isDigits(const char* s) {
  if (!s || !*s) return false;
  for (const char* p = s; *p; ++p) {
    if (*p < '0' || *p > '9') return false;
  }
  return true;
}

}  // namespace

std::optional<ProcessIdentity> readProcessIdentity(int pid) {
  ProcessIdentity id;
  std::uint64_t rssBytes = 0;
  if (!readProcRssBytes(pid, rssBytes)) return std::nullopt;

  std::uint64_t procJiffies = 0;
  std::string name;
  if (!readProcStatTimes(pid, procJiffies, name)) return std::nullopt;

  id.name = name;
  id.ramBytes = rssBytes;
  return id;
}

std::vector<CpuProcessInfo> ProcessSampler::sampleTop(std::size_t maxCount) {
  std::vector<CpuProcessInfo> out;
  if (maxCount == 0) return out;

  std::uint64_t totalJiffies = 0;
  if (!readTotalJiffies(totalJiffies)) return out;

  const std::uint64_t deltaTotal = hasPrev_ ? (totalJiffies - prevTotalJiffies_) : 0;

  std::unordered_map<int, std::uint64_t> curProcJiffies;
  curProcJiffies.reserve(1024);

  DIR* dir = ::opendir("/proc");
  if (!dir) return out;

  struct dirent* ent = nullptr;
  while ((ent = ::readdir(dir)) != nullptr) {
    if (ent->d_type != DT_DIR && ent->d_type != DT_LNK && ent->d_type != DT_UNKNOWN) continue;
    if (!isDigits(ent->d_name)) continue;
    const int pid = std::atoi(ent->d_name);
    if (pid <= 0) continue;

    std::uint64_t procJiffies = 0;
    std::string name;
    if (!readProcStatTimes(pid, procJiffies, name)) continue;

    std::uint64_t rssBytes = 0;
    if (!readProcRssBytes(pid, rssBytes)) continue;

    curProcJiffies[pid] = procJiffies;

    double cpuPct = 0.0;
    if (hasPrev_ && deltaTotal > 0) {
      auto it = prevProcJiffies_.find(pid);
      if (it != prevProcJiffies_.end()) {
        const std::uint64_t deltaProc = procJiffies - it->second;
        cpuPct = 100.0 * (static_cast<double>(deltaProc) / static_cast<double>(deltaTotal));
      }
    }

    out.push_back(CpuProcessInfo{pid, name, cpuPct, rssBytes});
  }

  ::closedir(dir);

  prevTotalJiffies_ = totalJiffies;
  prevProcJiffies_ = std::move(curProcJiffies);
  hasPrev_ = true;

  std::sort(out.begin(), out.end(), [](const CpuProcessInfo& a, const CpuProcessInfo& b) {
    if (a.cpuPct != b.cpuPct) return a.cpuPct > b.cpuPct;
    return a.pid < b.pid;
  });

  if (out.size() > maxCount) out.resize(maxCount);
  return out;
}

}  // namespace aiz
