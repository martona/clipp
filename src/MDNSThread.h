#pragma once

#include <cstdint>

// Callback type: receives discovered host metadata and network name.
using MDNSCallback = void(*)(const char* hostNameUtf8, 
							const char* hostIDHex, 
							const char* senderIpUtf8, 
							const char* queryIDHex, 
							const char* nonceHex, 
							const char* verbUtf8, 
							unsigned short port, 
							const unsigned char* rawHostID);

// Starts the mDNS thread. Returns true on success.
bool StartMDNS(MDNSCallback callback);

// Notify mDNS that the network key has changed. The next broadcast is sent immediately.
void MDNSNotifyNetworkKeyChange();

// Stops the mDNS thread. Blocks until the thread exits.
void StopMDNS();
