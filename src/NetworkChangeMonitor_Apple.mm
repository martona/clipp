#include "platform.h"

#ifdef __APPLE__

#include "NetworkChangeMonitor.h"

#import <Network/Network.h>
#include <dispatch/dispatch.h>

#include <functional>
#include <utility>

#if !__has_feature(objc_arc)
#error "NetworkChangeMonitor_Apple.mm requires ARC. Enable CLANG_ENABLE_OBJC_ARC on the target."
#endif

namespace clipp {

struct NetworkChangeMonitor {
    nw_path_monitor_t monitor = nil;   // ARC-managed
    dispatch_queue_t queue = nil;      // ARC-managed
    std::function<void()> onChange;
};

NetworkChangeMonitor* StartNetworkChangeMonitor(std::function<void()> onChange) {
    auto* mon = new NetworkChangeMonitor();
    mon->onChange = std::move(onChange);
    mon->queue = dispatch_queue_create("com.clipp.netmonitor", DISPATCH_QUEUE_SERIAL);
    mon->monitor = nw_path_monitor_create();
    if (mon->monitor == nil || mon->queue == nil) {
        delete mon;
        return nullptr;
    }

    // Capture a COPY of the callback, not `mon`: nw_path_monitor may deliver a final
    // update after StopNetworkChangeMonitor deletes `mon`, but the callback targets the
    // process-lifetime NetworkRuntime, so invoking the copy is always safe.
    std::function<void()> cb = mon->onChange;
    nw_path_monitor_set_queue(mon->monitor, mon->queue);
    nw_path_monitor_set_update_handler(mon->monitor, ^(nw_path_t path) {
        (void)path;
        cb();
    });
    nw_path_monitor_start(mon->monitor);
    return mon;
}

void StopNetworkChangeMonitor(NetworkChangeMonitor* mon) {
    if (mon == nullptr) return;
    if (mon->monitor != nil) {
        nw_path_monitor_cancel(mon->monitor);
    }
    mon->monitor = nil;   // ARC release
    mon->queue = nil;     // ARC release
    delete mon;
}

}  // namespace clipp

#endif  // __APPLE__
