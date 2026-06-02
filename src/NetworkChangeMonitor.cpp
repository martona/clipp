#include "platform.h"

#include "NetworkChangeMonitor.h"

#include <functional>
#include <utility>

#if defined(_WIN32)

#include <iphlpapi.h>
#pragma comment(lib, "iphlpapi.lib")

namespace clipp {

struct NetworkChangeMonitor {
    HANDLE handle = nullptr;
    std::function<void()> onChange;
};

// NotifyUnicastIpAddressChange delivers this on a system threadpool thread for any
// add/remove/parameter change of a unicast address. We ignore the row/type -- it's just
// a "re-check" hint -- and signal the worker.
static void NETIOAPI_API_ ClippAddressChangeCallback(PVOID context,
                                                     PMIB_UNICASTIPADDRESS_ROW /*row*/,
                                                     MIB_NOTIFICATION_TYPE /*type*/) {
    auto* monitor = static_cast<NetworkChangeMonitor*>(context);
    if (monitor && monitor->onChange) {
        monitor->onChange();
    }
}

NetworkChangeMonitor* StartNetworkChangeMonitor(std::function<void()> onChange) {
    auto* monitor = new NetworkChangeMonitor();
    monitor->onChange = std::move(onChange);
    const DWORD status = NotifyUnicastIpAddressChange(
        AF_UNSPEC, ClippAddressChangeCallback, monitor, /*InitialNotification=*/FALSE, &monitor->handle);
    if (status != NO_ERROR) {
        delete monitor;
        return nullptr;
    }
    return monitor;
}

void StopNetworkChangeMonitor(NetworkChangeMonitor* monitor) {
    if (monitor == nullptr) return;
    // CancelMibChangeNotify2 waits for any in-flight callback to return, so no callback
    // can touch `monitor` after this; safe to delete. (Don't hold a lock the callback
    // wants while calling it -- we don't.)
    if (monitor->handle) {
        CancelMibChangeNotify2(monitor->handle);
    }
    delete monitor;
}

}  // namespace clipp

#elif defined(__APPLE__)

// Implemented in NetworkChangeMonitor_Apple.mm (nw_path_monitor; needs Network.framework
// and ARC, so it can't live in this plain .cpp).

#else  // Linux / other

// The headless Linux build compiles NetworkRuntime out entirely, so this is never called;
// kept as a defined stub so the contract is satisfied and the TU isn't empty.
namespace clipp {

struct NetworkChangeMonitor {};

NetworkChangeMonitor* StartNetworkChangeMonitor(std::function<void()> /*onChange*/) {
    return nullptr;
}

void StopNetworkChangeMonitor(NetworkChangeMonitor* /*monitor*/) {}

}  // namespace clipp

#endif
