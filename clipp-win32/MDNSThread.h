#pragma once

// Callback type: receives discovered host name, sender IP, queryID, and nonce (all as strings)
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

// Stops the mDNS thread. Blocks until the thread exits.
void StopMDNS();
