#include "MDNSThread.h"
#include <thread>
#include <future>
#include <chrono>
#include <cstring>
#include <mdns.h>
#include <iostream>

static std::thread g_mdnsThread;
static MDNSCallback g_mdnsCallback = nullptr;
static int g_mdnsSock = -1;

int mdns_record_callback(int sock, const struct sockaddr* from, size_t addrlen,
    mdns_entry_type_t entry, uint16_t query_id, uint16_t rtype,
    uint16_t rclass, uint32_t ttl, const void* data, size_t size,
    size_t name_offset, size_t name_length, size_t record_offset,
    size_t record_length, void* user_data)
{
    std::cout << "MDNS!" << std::endl;
    if (g_mdnsCallback) 
        g_mdnsCallback("host");
    return 0;
}


static void MDNSThreadProc(std::promise<bool> initPromise, MDNSCallback callback) {
    g_mdnsCallback = callback;
    g_mdnsSock = mdns_socket_open_ipv4(0);
    if (g_mdnsSock < 0) {
        initPromise.set_value(false);
        return;
    }
    initPromise.set_value(true);
    return;
}

bool StartMDNS(MDNSCallback callback) {
    std::promise<bool> initPromise;
    std::future<bool> initFuture = initPromise.get_future();
    g_mdnsThread = std::thread(MDNSThreadProc, std::move(initPromise), callback);
    if (!initFuture.get()) {
        if (g_mdnsThread.joinable())
            g_mdnsThread.join();
        return false;
    }
    return true;
}

void StopMDNS() {
    if (g_mdnsSock >= 0) {
        mdns_socket_close(g_mdnsSock);
        g_mdnsSock = -1;
    }
    if (g_mdnsThread.joinable())
        g_mdnsThread.join();
}
