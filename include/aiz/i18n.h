#pragma once

#include <string_view>
#include <string>

namespace aiz::i18n {

// Keep this intentionally tiny: compiled-in tables only.
// UI strings only (logs/errors remain English).
enum class MsgId {
  FooterNav,

  FooterViewTimelines,
  FooterViewBars,
  FooterViewMinimal,

  ScreenHelpTitle,
  ScreenConfigTitle,
  ScreenHardwareTitle,
  ScreenBenchmarksTitle,
  ScreenProcessesTitle,

  TimelinesTitle,
  TimelinesNoneEnabled,

  ConfigTitle,
  ConfigSectionTimelines,
  ConfigSectionBars,
  ConfigSectionMisc,

  ConfigToggleCpuUsage,
  ConfigToggleCpuHotCoreUsage,
  ConfigToggleGpuUsage,
  ConfigToggleGpuMemCtrl,
  ConfigToggleGpuClock,
  ConfigToggleGpuMemClock,
  ConfigToggleGpuEnc,
  ConfigToggleGpuDec,
  ConfigToggleDiskRead,
  ConfigToggleDiskWrite,
  ConfigToggleNetRx,
  ConfigToggleNetTx,
  ConfigTogglePcieRx,
  ConfigTogglePcieTx,
  ConfigToggleRamUsage,
  ConfigToggleVramUsage,

  ConfigToggleOn,
  ConfigToggleOff,
  ConfigTogglePeakValues,
  ConfigReadonlyPeakWindow,
  ConfigResetToDefaults,
  ConfigResetTag,
  ConfigReadonlySamplesPerBucket,
  ConfigReadonlySamplingRate,
  ConfigReadonlyValueColor,
  ConfigReadonlyMetricNameColor,
  ConfigReadonlyGraphStyle,
  ConfigFooterKeys,

  HardwareFooterKeys,

  BenchRunAll,
  BenchUnavailable,
  BenchFooterKeys,
};

enum class Lang {
  En,
  ZhCN,
};

// Accepts tags like: "en", "zh", "zh-CN", "zh_CN", "zh-Hans".
void setLanguageTag(std::string_view langTag);
Lang language();

// Returns a stable view to a compiled-in wide string.
std::wstring_view tr(MsgId id);

}  // namespace aiz::i18n
