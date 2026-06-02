#pragma once

#include <functional>

// OS-driven watch for local network interface / address changes, so the runtime can
// re-announce DNS-SD only when something actually changes instead of polling. The
// callback is a coarse "re-check" hint (it fires for path/route/DNS changes too, not
// just address changes the app cares about); the caller re-hashes the interface set to
// decide whether a republish is warranted.

namespace clipp {

// Opaque, platform-specific handle.
struct NetworkChangeMonitor;

// Starts the watch. `onChange` is invoked on an OS-internal thread whenever the local
// network configuration may have changed; it MUST be cheap and non-blocking -- just
// signal a worker. Returns nullptr if no event source could be started (caller then
// runs without auto-republish on interface change). Stop with StopNetworkChangeMonitor.
NetworkChangeMonitor* StartNetworkChangeMonitor(std::function<void()> onChange);

// Stops the watch and frees the handle. Blocks until no callback is in flight, so it is
// safe to tear down state the callback touches once this returns. Null-safe.
void StopNetworkChangeMonitor(NetworkChangeMonitor* monitor);

}  // namespace clipp
