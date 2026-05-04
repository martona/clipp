#pragma once

// Callback type: receives discovered host name, sender IP, queryID, and nonce (all as strings)
using MDNSCallback = void(*)(const wchar_t* hostName, const wchar_t* hostID, const wchar_t* senderIp, const wchar_t* queryID, const wchar_t* nonce, const wchar_t* verb, unsigned short port, const unsigned char* rawHostID);

// Starts the mDNS thread. Returns true on success.
bool StartMDNS(MDNSCallback callback);

// Stops the mDNS thread. Blocks until the thread exits.
void StopMDNS();
