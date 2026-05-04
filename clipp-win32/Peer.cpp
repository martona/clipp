#include "Logger.h"
#include "Peer.h"

#include <array>
#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>
#include <ws2tcpip.h>

#include "Settings.h"

namespace {
#pragma pack(push, 1)
struct ClientHello {
	wchar_t selector[8];
	unsigned short version;
	unsigned char hostID[32];
	wchar_t hostName[256];
};
#pragma pack(pop)

constexpr const wchar_t* kSelector = L"clipp";
constexpr unsigned short kVersion = 1;
}

Peer::Peer(const wchar_t* hostName, const unsigned char* hostID, const char* ip, u_short port)
	: hostName_(hostName), ip_(ip), port_(port),
	createdAt_(std::chrono::steady_clock::now()),
	lastPingReceivedAt_(createdAt_) {
	std::memcpy(hostID_.data(), hostID, hostID_.size());
}

Peer::~Peer() {
	Stop();
}

void Peer::Start() {
	thread_ = std::thread(&Peer::ThreadProc, this);
}

void Peer::Stop() {
	stopRequested_.store(true);
	CloseSocket();
	if (thread_.joinable()) {
		thread_.join();
	}
}

std::wstring Peer::hostName() const {
	std::lock_guard<std::mutex> lock(dataMutex_);
	return hostName_;
}

std::array<unsigned char, 32> Peer::hostID() const {
	std::lock_guard<std::mutex> lock(dataMutex_);
	return hostID_;
}

std::wstring Peer::ipw() const {
	std::lock_guard<std::mutex> lock(dataMutex_);
	return std::wstring(ip_.begin(), ip_.end());
}

std::string Peer::ip() const {
	std::lock_guard<std::mutex> lock(dataMutex_);
	return ip_;
}

unsigned short Peer::port() const {
	std::lock_guard<std::mutex> lock(dataMutex_);
	return port_;
}

std::chrono::steady_clock::time_point Peer::lastPingReceivedAt() const {
	std::lock_guard<std::mutex> lock(dataMutex_);
	return lastPingReceivedAt_;
}

std::chrono::steady_clock::time_point Peer::createdAt() const {
	std::lock_guard<std::mutex> lock(dataMutex_);
	return createdAt_;
}

bool Peer::RecvAll(SOCKET sock, char* buffer, int length) {
	int total = 0;
	while (total < length) {
		const int received = recv(sock, buffer + total, length - total, 0);
		if (received <= 0) {
			return false;
		}
		total += received;
	}
	return true;
}

bool Peer::SendAll(SOCKET sock, const char* buffer, int length) {
	int total = 0;
	while (total < length) {
		const int sent = send(sock, buffer + total, length - total, 0);
		if (sent <= 0) {
			return false;
		}
		total += sent;
	}
	return true;
}

void Peer::CloseSocket() {
	if (socket_ != INVALID_SOCKET) {
		shutdown(socket_, SD_BOTH);
		closesocket(socket_);
		socket_ = INVALID_SOCKET;
	}
}

bool Peer::ConnectSocket() {
	SOCKET socketHandle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (socketHandle == INVALID_SOCKET) {
		g_logger.log(__FUNCTION__, Logger::Level::Error, L"Peer: failed to create socket.");
		return false;
	}

	sockaddr_in address{};
	address.sin_family = AF_INET;
	address.sin_port = htons(port());
	std::string peerIp = ip();
	if (inet_pton(AF_INET, peerIp.c_str(), &address.sin_addr) != 1) {
		g_logger.log(__FUNCTION__, Logger::Level::Error, L"Peer: invalid remote IP address.");
		closesocket(socketHandle);
		return false;
	}

	if (connect(socketHandle, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR) {
		g_logger.log(__FUNCTION__, Logger::Level::Warning, L"Peer: TCP connect failed; retrying.");
		closesocket(socketHandle);
		return false;
	}

	socket_ = socketHandle;
	return true;
}

bool Peer::SendHello() {
	ClientHello hello{};
	wcsncpy_s(hello.selector, _countof(hello.selector), kSelector, _TRUNCATE);
	hello.version = htons(kVersion);

	std::array<unsigned char, 32> localHostId{};
	if (!g_settings.getHostID(localHostId)) {
		g_logger.log(__FUNCTION__, Logger::Level::Error, L"Peer: failed to read local host ID.");
		return false;
	}
	std::memcpy(hello.hostID, localHostId.data(), localHostId.size());

	char localHostName[256] = {};
	if (gethostname(localHostName, sizeof(localHostName)) != 0) {
		g_logger.log(__FUNCTION__, Logger::Level::Error, L"Peer: gethostname failed.");
		return false;
	}
	wchar_t localHostNameW[256] = {};
	mbstowcs_s(nullptr, localHostNameW, _countof(localHostNameW), localHostName, _TRUNCATE);
	wcsncpy_s(hello.hostName, _countof(hello.hostName), localHostNameW, _TRUNCATE);

	if (socket_ == INVALID_SOCKET) {
		return false;
	}
	return SendAll(socket_, reinterpret_cast<const char*>(&hello), sizeof(hello));
}

void Peer::ThreadProc() {
	while (!stopRequested_.load()) {
		if (!ConnectSocket()) {
			g_logger.log(__FUNCTION__, Logger::Level::Error, L"Peer: failed to connect.");
			if (!stopRequested_.load()) std::this_thread::sleep_for(std::chrono::seconds(5));
			continue;
		}
		if (!SendHello()) {
			g_logger.log(__FUNCTION__, Logger::Level::Error, L"Peer: failed sending login handshake.");
			CloseSocket();
			if (!stopRequested_.load()) std::this_thread::sleep_for(std::chrono::seconds(5));
			continue;
		}

		g_logger.log(__FUNCTION__, Logger::Level::Info, L"Peer connected and authenticated.");
		while (!stopRequested_.load()) {
			{
				if (socket_ == INVALID_SOCKET || !SendAll(socket_, "PING", 4)) {
					g_logger.log(__FUNCTION__, Logger::Level::Warning, L"Peer failed SendAll");
					break;
				}
			}

			char packet[4] = {};
			{
				if (socket_ == INVALID_SOCKET || !RecvAll(socket_, packet, 4)) {
					g_logger.log(__FUNCTION__, Logger::Level::Warning, L"Peer failed RecvAll");
					break;
				}
			}

			if (std::memcmp(packet, "PONG", 4) != 0) {
				g_logger.log(__FUNCTION__, Logger::Level::Error, L"Peer: unexpected packet received.");
				break;
			}

			{
				std::lock_guard<std::mutex> lock(dataMutex_);
				lastPingReceivedAt_ = std::chrono::steady_clock::now();
				g_logger.log(__FUNCTION__, Logger::Level::Debug, L"Peer: PONG");
			}

			fd_set readSet;
			FD_ZERO(&readSet);
			FD_SET(socket_, &readSet);
			timeval timeout{};
			timeout.tv_sec = 10;
			int selresult = select(0, &readSet, nullptr, nullptr, &timeout);
			g_logger.log(__FUNCTION__, Logger::Level::Debug, L"Looping (inner)");
		}
		g_logger.log(__FUNCTION__, Logger::Level::Debug, L"Closing socket");
		CloseSocket();
		if (!stopRequested_.load()) std::this_thread::sleep_for(std::chrono::seconds(5));
		g_logger.log(__FUNCTION__, Logger::Level::Debug, L"Looping (outer)");
	}
	g_logger.log(__FUNCTION__, Logger::Level::Debug, L"Peer thread exiting.");
}
