#include <aiz/tui/win32_terminal_ui.h>

namespace aiz {

std::unique_ptr<Ui> makeUi() {
  return std::make_unique<Win32TerminalUi>();
}

}  // namespace aiz
