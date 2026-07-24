#pragma once

// Daemon-side wiring for RegisterPersistence: resolves the sealing subkey and
// the group-scoped file path, loads the snapshot into g_registerStore at
// startup, and runs the debounced background writer. All functions are no-ops
// on builds without a register daemon (CLIPP_REGISTERS_DAEMON == 0).
namespace clipp {

// Call once at daemon startup — after the register store is configured
// (SetLocalHost/SetLimits/SeedClockFloor) and the network key has been probed,
// but BEFORE network listeners start: the first inbound RSYN must run against
// the loaded store, not an empty one (which would re-pull everything). With no
// key yet (pre-pairing) the store stays RAM-only until the first join reports
// in via RegisterPersistenceKeyChanged.
void StartRegisterPersistence();

// The network key was set, changed, or cleared mid-session (pairing UI). Any
// pending state is flushed to the OLD group's file first, then the file swaps
// to the new key's fingerprint and its stored records merge in.
void RegisterPersistenceKeyChanged();

// Shutdown: final flush of a dirty store, then the writer thread joins. Call
// after the network stack has stopped (so the last inbound records are in).
void StopRegisterPersistence();

}  // namespace clipp
