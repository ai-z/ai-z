#pragma once

#include <aiz/tui/ui.h>

namespace aiz {

// Full-screen Win32 console UI using screen buffers + ReadConsoleInput.
class Win32TerminalUi final : public Ui {
public:
  int run(Config& cfg, bool debugMode) override;
};

}  // namespace aiz
