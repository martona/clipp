#pragma once

#include <optional>

// Initializes libsodium. Defined in Cli.cpp and shared by both the command-line
// path and the GUI startup in main(), so there is exactly one definition and the
// process initializes sodium exactly once on whichever path runs.
bool InitializeSodium();

namespace clipp::cli {

// Parses argv. If a recognized subcommand (key/hostid) is present, runs it and
// returns its process exit code (the caller should return it from main). If none
// is present, applies any global options (e.g. --loglevel) and returns
// std::nullopt so the caller proceeds to the GUI.
//
// Diagnostic logging is silenced (Logger::Level::Off) while a subcommand runs
// unless --loglevel is given; command results go to stdout and command errors to
// stderr, independent of the log stream.
std::optional<int> Run(int argc, char** argv);

}  // namespace clipp::cli
