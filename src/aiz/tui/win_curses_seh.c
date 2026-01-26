#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <curses.h>

// Wrap refresh() with SEH so an access violation in some Windows terminal backends
// doesn't take down the whole process without a message.
int aiz_windows_safe_refresh(void) {
  __try {
    refresh();
    return 1;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return 0;
  }
}
#endif
