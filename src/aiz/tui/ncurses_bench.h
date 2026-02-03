#pragma once

#include <aiz/tui/tui_core.h>

#include <thread>

namespace aiz::ncurses {

void benchJoinIfDone(std::thread& benchThread, TuiState& state);

// Handles `Command::Activate` on the Benchmarks screen.
// Preserves the existing semantics:
// - If a bench run is in progress, activation is ignored.
// - If a prior worker finished, it is joined before starting a new one.
// - If selection is a header/non-runnable row, sets lastBenchResult accordingly.
void benchHandleActivate(std::thread& benchThread, TuiState& state);

// Generates a static HTML report of benchmark results (runs all benches if needed).
// Returns the output path on success, or std::nullopt on failure.
std::optional<std::string> benchGenerateHtmlReport(std::thread& benchThread, TuiState& state);

void benchShutdown(std::thread& benchThread);

}  // namespace aiz::ncurses
