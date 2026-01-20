#pragma once

#include <aiz/hw/hardware_info.h>
#include <aiz/tui/tui_core.h>

namespace aiz::ncurses {

void rebuildBenchRows(TuiState& state, const HardwareInfo& hw, unsigned int gpuCount);

}  // namespace aiz::ncurses
