#include <aiz/i18n.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <string>

namespace aiz::i18n {

namespace {
std::atomic<Lang> g_lang{Lang::En};

std::string normalize(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (unsigned char ch : s) {
    if (ch == '_') {
      out.push_back('-');
      continue;
    }
    out.push_back(static_cast<char>(std::tolower(ch)));
  }
  return out;
}

bool isZhCnTag(const std::string& tag) {
  // Be liberal: users may pass zh, zh-cn, zh-hans, zh_cn.UTF-8, etc.
  if (tag.rfind("zh", 0) != 0) return false;
  if (tag == "zh") return true;
  if (tag.rfind("zh-", 0) == 0) return true;
  return false;
}

constexpr std::wstring_view en(MsgId id) {
  switch (id) {
    case MsgId::FooterNav:
      return L"ESCMainF1HelpF2HardwareF3BenchmarkF4ConfigF5ProcessesF10Quit View: 1Timelines 2H. Bars 3Minimal";

    case MsgId::FooterViewTimelines: return L"Timelines";
    case MsgId::FooterViewBars: return L"H. Bars";
    case MsgId::FooterViewMinimal: return L"Minimal";

    case MsgId::ScreenHelpTitle: return L"Help";
    case MsgId::ScreenConfigTitle: return L"Config";
    case MsgId::ScreenHardwareTitle: return L"Hardware";
    case MsgId::ScreenBenchmarksTitle: return L"Benchmarks";
    case MsgId::ScreenProcessesTitle: return L"Processes";

    case MsgId::TimelinesTitle: return L"Timelines";
    case MsgId::TimelinesNoneEnabled:
      return L"No timelines enabled. Use Config (F4 / C) to enable.";

    case MsgId::ConfigTitle: return L"Config";
    case MsgId::ConfigSectionTimelines: return L"Timelines";
    case MsgId::ConfigSectionBars: return L"H. Bars";
    case MsgId::ConfigSectionMisc: return L"Misc";

    case MsgId::ConfigToggleCpuUsage: return L"CPU usage";
    case MsgId::ConfigToggleCpuHotCoreUsage: return L"CPU hot core usage";
    case MsgId::ConfigToggleGpuUsage: return L"GPU usage";
    case MsgId::ConfigToggleGpuMemCtrl: return L"GPU mem ctrl";
    case MsgId::ConfigToggleGpuClock: return L"GPU MHz";
    case MsgId::ConfigToggleGpuMemClock: return L"Mem MHz";
    case MsgId::ConfigToggleGpuEnc: return L"GPU encoder";
    case MsgId::ConfigToggleGpuDec: return L"GPU decoder";
    case MsgId::ConfigToggleDiskRead: return L"Disk Read";
    case MsgId::ConfigToggleDiskWrite: return L"Disk Write";
    case MsgId::ConfigToggleNetRx: return L"Net RX";
    case MsgId::ConfigToggleNetTx: return L"Net TX";
    case MsgId::ConfigTogglePcieRx: return L"PCIe RX";
    case MsgId::ConfigTogglePcieTx: return L"PCIe TX";
    case MsgId::ConfigToggleRamUsage: return L"RAM usage";
    case MsgId::ConfigToggleVramUsage: return L"VRAM usage";

    case MsgId::ConfigToggleOn: return L"ON";
    case MsgId::ConfigToggleOff: return L"OFF";
    case MsgId::ConfigTogglePeakValues: return L"Show peak values";
    case MsgId::ConfigReadonlyPeakWindow: return L"Peak window";
    case MsgId::ConfigResetToDefaults: return L"Reset to Defaults";
    case MsgId::ConfigResetTag: return L"RESET";
    case MsgId::ConfigReadonlySamplesPerBucket: return L"Samples per bucket (bars)";
    case MsgId::ConfigReadonlySamplingRate: return L"Sampling rate";
    case MsgId::ConfigReadonlyValueColor: return L"Value color";
    case MsgId::ConfigReadonlyMetricNameColor: return L"Metric name color";
    case MsgId::ConfigReadonlyGraphStyle: return L"Graph style";
    case MsgId::ConfigFooterKeys:
      return L"Space: toggle   Tab/Left/Right: column   S: save   R: reset   Esc: back";

    case MsgId::HardwareFooterKeys:
      return L"r: refresh   Esc: back";

    case MsgId::BenchRunAll: return L"Run all benchmarks";
    case MsgId::BenchUnavailable: return L"unavailable";
    case MsgId::BenchFooterKeys: return L"Enter: run   Esc: back";
  }
  return L"";
}

constexpr std::wstring_view zhCN(MsgId id) {
  switch (id) {
    case MsgId::FooterNav:
      return L"ESCMainF1帮助F2硬件F3基准F4配置F5进程F10退出 视图: 1时间线 2柱状 3简洁";

    case MsgId::FooterViewTimelines: return L"时间线";
    case MsgId::FooterViewBars: return L"柱状";
    case MsgId::FooterViewMinimal: return L"简洁";

    case MsgId::ScreenHelpTitle: return L"帮助";
    case MsgId::ScreenConfigTitle: return L"配置";
    case MsgId::ScreenHardwareTitle: return L"硬件";
    case MsgId::ScreenBenchmarksTitle: return L"基准测试";
    case MsgId::ScreenProcessesTitle: return L"进程";

    case MsgId::TimelinesTitle: return L"时间线";
    case MsgId::TimelinesNoneEnabled:
      return L"未启用任何时间线。请在配置（F4 / C）中启用。";

    case MsgId::ConfigTitle: return L"配置";
    case MsgId::ConfigSectionTimelines: return L"时间线";
    case MsgId::ConfigSectionBars: return L"柱状";
    case MsgId::ConfigSectionMisc: return L"其他";

    case MsgId::ConfigToggleCpuUsage: return L"CPU 使用率";
    case MsgId::ConfigToggleCpuHotCoreUsage: return L"CPU 最热核心使用率";
    case MsgId::ConfigToggleGpuUsage: return L"GPU 使用率";
    case MsgId::ConfigToggleGpuMemCtrl: return L"GPU 显存控制";
    case MsgId::ConfigToggleGpuClock: return L"GPU 频率";
    case MsgId::ConfigToggleGpuMemClock: return L"显存频率";
    case MsgId::ConfigToggleGpuEnc: return L"GPU 编码";
    case MsgId::ConfigToggleGpuDec: return L"GPU 解码";
    case MsgId::ConfigToggleDiskRead: return L"磁盘读取";
    case MsgId::ConfigToggleDiskWrite: return L"磁盘写入";
    case MsgId::ConfigToggleNetRx: return L"网络接收";
    case MsgId::ConfigToggleNetTx: return L"网络发送";
    case MsgId::ConfigTogglePcieRx: return L"PCIe 接收";
    case MsgId::ConfigTogglePcieTx: return L"PCIe 发送";
    case MsgId::ConfigToggleRamUsage: return L"内存使用率";
    case MsgId::ConfigToggleVramUsage: return L"显存使用率";

    case MsgId::ConfigToggleOn: return L"开";
    case MsgId::ConfigToggleOff: return L"关";
    case MsgId::ConfigTogglePeakValues: return L"显示峰值";
    case MsgId::ConfigReadonlyPeakWindow: return L"峰值窗口";
    case MsgId::ConfigResetToDefaults: return L"恢复默认设置";
    case MsgId::ConfigResetTag: return L"重置";
    case MsgId::ConfigReadonlySamplesPerBucket: return L"每桶采样（柱）";
    case MsgId::ConfigReadonlySamplingRate: return L"采样间隔";
    case MsgId::ConfigReadonlyValueColor: return L"数值颜色";
    case MsgId::ConfigReadonlyMetricNameColor: return L"指标名称颜色";
    case MsgId::ConfigReadonlyGraphStyle: return L"图表样式";
    case MsgId::ConfigFooterKeys:
      return L"Space：切换   Tab/Left/Right：列   S：保存   R：重置   Esc：返回";

    case MsgId::HardwareFooterKeys:
      return L"r：刷新   Esc：返回";

    case MsgId::BenchRunAll: return L"运行全部基准测试";
    case MsgId::BenchUnavailable: return L"不可用";
    case MsgId::BenchFooterKeys: return L"Enter：运行   Esc：返回";
  }
  return L"";
}

}  // namespace

void setLanguageTag(std::string_view langTag) {
  const std::string tag = normalize(langTag);
  if (isZhCnTag(tag)) {
    g_lang.store(Lang::ZhCN);
  } else {
    g_lang.store(Lang::En);
  }
}

Lang language() {
  return g_lang.load();
}

std::wstring_view tr(MsgId id) {
  if (language() == Lang::ZhCN) {
    const auto s = zhCN(id);
    if (!s.empty()) return s;
  }
  return en(id);
}

}  // namespace aiz::i18n
