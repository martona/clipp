#pragma once

#include <optional>

// Initializes libsodium. Defined in Cli.cpp and shared by both the command-line
// path and the GUI startup in main(), so there is exactly one definition and the
// process initializes sodium exactly once on whichever path runs.
bool InitializeSodium();

namespace clipp::cli {

// Parses argv and decides how the process should proceed:
//   * a recognized command (copy/paste/key/hostid) runs and its process exit code
//     is returned (the caller should return it from main);
//   * a bare launch from a console (launchedFromConsole == true) prints usage and
//     returns 0 -- friendlier than silently launching the terminal-blocking GUI;
//   * a bare launch with no console, or the explicit `gui` subcommand, returns
//     std::nullopt so the caller proceeds to the GUI.
//
// launchedFromConsole is the result of InitializeConsoleOutput(): true when we
// were started from a terminal/shell, false for a pure GUI launch.
//
// Diagnostic logging is silenced (Logger::Level::Off) while a command runs unless
// --loglevel is given; command results go to stdout and command errors to stderr,
// independent of the log stream.
std::optional<int> Run(int argc, char** argv, bool launchedFromConsole);

}  // namespace clipp::cli
