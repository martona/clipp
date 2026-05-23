#pragma once

#include <cstdint>
#include "HostId.h"

// Callback type: receives discovered host metadata and network name.
using MDNSCallback = void(*)(const char* hostNameUtf8, 
							const char* senderIpUtf8, 
							const char* queryIDHex, 
							const char* nonceHex, 
							const char* verbUtf8, 
							unsigned short port, 
							const HostId& remoteHostID);

// Starts the mDNS thread. Returns true on success.
bool StartMDNS(MDNSCallback callback);

// Notify mDNS that the network key has changed. The next broadcast is sent immediately.
void MDNSNotifyNetworkKeyChange();

// Notify mDNS that the local host ID changed. The next broadcast is sent immediately.
void MDNSNotifyHostIDChange();

bool MDNSHasHostIDCollisionWarning();
void MDNSClearHostIDCollisionWarning();

// Stops the mDNS thread. Blocks until the thread exits.
void StopMDNS();
