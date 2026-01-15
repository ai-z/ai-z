#include <aiz/tui/ui.h>

#include <aiz/tui/ncurses_ui.h>

#include <memory>

namespace aiz {

std::unique_ptr<Ui> makeUi() {
  return std::make_unique<NcursesUi>();
}

}  // namespace aiz
