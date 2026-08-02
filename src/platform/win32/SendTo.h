#pragma once

#include <optional>

// Explorer "Send To -> Clipp" integration (Windows-only): a Clipp.lnk in the
// per-user SendTo folder that relaunches this executable with
// `--sendto <file>...`. Registration mirrors the autostart lifecycle:
// refreshed on every GUI startup (self-heals a moved/updated exe path),
// removed on manual exit.
bool RegisterClippSendTo();
bool UnregisterClippSendTo();

// If the process was launched with --sendto, relays the named files to the mesh
// (one-shot gateway push, same as `clipp copy` / the iOS share extension) and
// returns the process exit code. Returns nullopt on a normal launch, in which
// case the caller proceeds to the GUI. Must run BEFORE the single-instance
// gate: the helper neither disturbs nor is blocked by the running tray daemon.
std::optional<int> RunSendToIfRequested();
