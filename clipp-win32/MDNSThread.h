#pragma once

using MDNSCallback = void(*)(const char* hostName);

// Starts the mDNS thread. Returns true on success.
bool StartMDNS(MDNSCallback callback);

// Stops the mDNS thread. Blocks until the thread exits.
void StopMDNS();
