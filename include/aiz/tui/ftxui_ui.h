#pragma once

#include <aiz/config/config.h>
#include <aiz/tui/ui.h>

namespace aiz {

class FtxuiUi : public Ui {
public:
  int run(Config& cfg, bool debugMode) override;
};

}  // namespace aiz
