
#include <windows.h>
#include <string>
#include "Peer.h"

Peer::Peer(const wchar_t* hostName, const unsigned char* hostID, const char* ip, u_short port) {
	hostName_ = hostName;
	memcpy(hostID_, hostID, sizeof(hostID_));
	ip_ = ip;
	port_ = port;
}

Peer::~Peer() {
}

const wchar_t* Peer::hostName() const {
	return hostName_.c_str();
}

const unsigned char* Peer::hostID() const {
	return hostID_;
}

std::wstring Peer::ipw() const {
	return std::wstring(ip_.begin(), ip_.end());
}

std::string Peer::ip() const {
	return ip_;
}

unsigned short Peer::port() const {
	return port_;
}
