#include "Logger.h"
#include "Peer.h"

#include <array>
#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>
#include <ws2tcpip.h>

#include "Settings.h"
#include "CryptoChannel.h"

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
	g_logger.log(__FUNCTION__, Logger::Level::Debug, L"%p Stopping Peer.", this);
	stopRequested_.store(true);
	stopCV_.notify_all();
	CloseSocket();
	if (thread_.joinable()) {
		thread_.join();
	}
}

void Peer::InterruptibleSleep(std::chrono::milliseconds duration) {
	std::unique_lock<std::mutex> lock(stopMutex_);
	stopCV_.wait_for(lock, duration, [this]() { return stopRequested_.load(); });
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
	return false;
}


void Peer::ThreadProc() {
	while (!stopRequested_.load()) {
		if (!ConnectSocket()) {
			g_logger.log(__FUNCTION__, Logger::Level::Error, L"Peer: failed to connect.");
			if (!stopRequested_.load()) InterruptibleSleep(std::chrono::milliseconds(5000));
			continue;
		}
		CryptoChannel channel;
		std::array<unsigned char, 32> remoteHostId{};
		std::string remoteHostNameUtf8;
		std::array<unsigned char, 32> localHostId{};
		if (!g_settings.getHostID(localHostId)) { CloseSocket(); continue; }
		char localHostNameA[256] = {};
		if (gethostname(localHostNameA, sizeof(localHostNameA)) != 0) { CloseSocket(); continue; }
		if (!channel.ClientHandshake(socket_, localHostId, localHostNameA, remoteHostId, remoteHostNameUtf8)) {
			g_logger.log(__FUNCTION__, Logger::Level::Error, L"Peer: secure handshake failed.");
			CloseSocket();
			if (!stopRequested_.load()) InterruptibleSleep(std::chrono::milliseconds(5000));
			continue;
		}

		g_logger.log(__FUNCTION__, Logger::Level::Info, L"%p Peer connected and authenticated.", this);
		while (!stopRequested_.load()) {
			if (socket_ == INVALID_SOCKET || !channel.SendTaggedMessage(socket_, "PING")) {
				g_logger.log(__FUNCTION__, Logger::Level::Warning, L"%p Peer failed secure send", this);
				break;
			}

			char packet[4] = {};
			if (socket_ == INVALID_SOCKET || !channel.RecvTaggedMessage(socket_, packet)) {
				g_logger.log(__FUNCTION__, Logger::Level::Warning, L"%p Peer failed secure recv", this);
				break;
			}

			if (std::memcmp(packet, "PONG", 4) != 0) {
				g_logger.log(__FUNCTION__, Logger::Level::Error, L"%p Peer: unexpected packet received.", this);
				break;
			}

			{
				std::lock_guard<std::mutex> lock(dataMutex_);
				lastPingReceivedAt_ = std::chrono::steady_clock::now();
				g_logger.log(__FUNCTION__, Logger::Level::Debug, L"%p Peer: PONG", this);
			}

			InterruptibleSleep(std::chrono::milliseconds(5000));
		}
		CloseSocket();
		if (!stopRequested_.load()) InterruptibleSleep(std::chrono::milliseconds(5000));
	}
	g_logger.log(__FUNCTION__, Logger::Level::Debug, L"%p Peer thread exiting.", this);
}
