#include <aiz/config/config.h>

#include "test_framework.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

static void setEnvVar(const char* key, const std::string& value) {
#if defined(_WIN32)
  _putenv_s(key, value.c_str());
#else
  setenv(key, value.c_str(), 1);
#endif
}

static fs::path makeTempDir() {
  const auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
  fs::path dir = fs::temp_directory_path() / ("ai-z-tests-" + std::to_string(static_cast<long long>(now)));
  fs::create_directories(dir);
  return dir;
}

TEST_CASE("Config::load returns defaults when no config exists") {
  const fs::path temp = makeTempDir();
  setEnvVar("XDG_CONFIG_HOME", temp.string());

  const aiz::Config cfg = aiz::Config::load();

  REQUIRE(cfg.showCpu == true);
  REQUIRE(cfg.showCpuBars == true);
  REQUIRE(cfg.showGpu == true);
  REQUIRE(cfg.showGpuBars == true);
  REQUIRE(cfg.showDiskReadBars == true);
  REQUIRE(cfg.showDiskWriteBars == true);
  REQUIRE(cfg.showNetRxBars == true);
  REQUIRE(cfg.showNetTxBars == true);
  REQUIRE(cfg.showRamBars == true);
  REQUIRE(cfg.showVramBars == true);
  REQUIRE(cfg.refreshMs == 500);
  REQUIRE(cfg.timelineSamples == 120);
  REQUIRE(cfg.timelineAgg == aiz::TimelineAgg::Max);
}

TEST_CASE("Config::load parses basic toggles and backward-compat keys") {
  const fs::path temp = makeTempDir();
  setEnvVar("XDG_CONFIG_HOME", temp.string());

  const fs::path cfgDir = temp / "ai-z";
  fs::create_directories(cfgDir);

  const fs::path cfgFile = cfgDir / "config.ini";
  std::ofstream out(cfgFile);
  out << "showCpu=false\n";
  out << "showDisk=false\n";
  out << "showNet=false\n";
  out << "timelineAgg=mean\n";
  out.close();

  const aiz::Config cfg = aiz::Config::load();

  REQUIRE(cfg.showCpu == false);
  // Bars toggles are independent and default to all-on unless set explicitly.
  REQUIRE(cfg.showCpuBars == true);

  // Backward-compat: showDisk controls both read+write.
  REQUIRE(cfg.showDisk == false);
  REQUIRE(cfg.showDiskRead == false);
  REQUIRE(cfg.showDiskWrite == false);
  REQUIRE(cfg.showDiskReadBars == true);
  REQUIRE(cfg.showDiskWriteBars == true);

  // Backward-compat: showNet controls both Rx+Tx.
  REQUIRE(cfg.showNetRx == false);
  REQUIRE(cfg.showNetTx == false);
  REQUIRE(cfg.showNetRxBars == true);
  REQUIRE(cfg.showNetTxBars == true);

  // Synonym parsing.
  REQUIRE(cfg.timelineAgg == aiz::TimelineAgg::Avg);
}
