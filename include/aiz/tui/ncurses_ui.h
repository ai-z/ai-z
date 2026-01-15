#pragma once

#include <aiz/config/config.h>
#include <aiz/tui/ui.h>

namespace aiz {

class NcursesUi : public Ui {
public:
  int run(Config& cfg, bool debugMode) override;
};

}  // namespace aiz
