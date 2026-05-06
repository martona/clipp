#include "Logger.h"
#include "Peer.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <limits>
#include <thread>

#include "Settings.h"
#include "CryptoChannel.h"
#include "NetworkDefs.h"
#include "utils.h"

Peer::Peer(const wchar_t* hostName, const unsigned char* hostID, const wchar_t* ip, u_short port)
	: hostName_(hostName), ip_(ip), port_(port),
	createdAt_(std::chrono::steady_clock::now()),
	lastPingReceivedAt_(createdAt_) {
	std::memcpy(hostID_.data(), hostID, hostID_.size());
}

void Peer::log(const char* function, Logger::Level level, const wchar_t* message, ...) const {
	va_list args;
	va_start(args, message);
	logV(function, level, message, args);
	va_end(args);
}

void Peer::logV(const char* function, Logger::Level level, const wchar_t* message, va_list args) const {
	wchar_t formattedMessage[1024];
	int bufferSize = cntof(formattedMessage);
	{
		std::lock_guard<std::mutex> lock(dataMutex_);
		int prefixlen = snwprintf_truncate(formattedMessage, bufferSize, L"[%ls %ls] ",
			hostName_.empty() ? L"<unknown>" : hostName_.c_str(),
			ip_.empty() ? L"<unknown>" : ip_.c_str());
		if (prefixlen < 0) return;
		vsnwprintf_truncate(formattedMessage + prefixlen, bufferSize - prefixlen, message != nullptr ? message : L"", args);
	}
	g_logger.log(function, level, L"%ls", formattedMessage);
}

Peer::~Peer() {
	Stop();
}

void Peer::Start() {
	thread_ = std::thread(&Peer::ThreadProc, this);
}

void Peer::Stop() {
	log(__FUNCTION__, Logger::Level::Debug, L"Stopping Peer.");
	stopRequested_.store(true);
	stopCV_.notify_all();
	messageQueue_.WakeAll();
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

std::wstring Peer::ip() const {
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
		log(__FUNCTION__, Logger::Level::Error, L"Peer: failed to create socket.");
		return false;
	}

	sockaddr_in address{};
	address.sin_family = AF_INET;
	address.sin_port = htons(port());
	std::string peerIp = WideToUtf8String(ip());
	if (inet_pton(AF_INET, peerIp.c_str(), &address.sin_addr) != 1) {
		log(__FUNCTION__, Logger::Level::Error, L"Peer: invalid remote IP address.");
		closesocket(socketHandle);
		return false;
	}

	if (connect(socketHandle, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR) {
		log(__FUNCTION__, Logger::Level::Debug, L"Peer: TCP connect failed; retrying.");
		closesocket(socketHandle);
		return false;
	}

	socket_ = socketHandle;
	return true;
}

bool Peer::SendHello() {
	return false;
}

bool Peer::SendClipboardData(CryptoChannel& channel, const ClipboardPayload& payload) {
	ClipboardPayload payloadToSend = payload;
	const size_t maxEncodedPayloadBytes = (64u * 1024u * 1024u) - sizeof(NetworkDefs::ClipboardMessage) - crypto_secretstream_xchacha20poly1305_ABYTES;
	if (payloadToSend.rawData.size() > maxEncodedPayloadBytes) return false;

	const uint32_t encodedDataSize = static_cast<uint32_t>(payloadToSend.rawData.size());
	uint32_t decodedDataSize = payloadToSend.decodedDataSize;
	if (decodedDataSize == 0 && !payloadToSend.isCompressed) {
		decodedDataSize = encodedDataSize;
	}

	NetworkDefs::ClipboardMessage msg{};
	msg.formatId = htonl(payloadToSend.formatId);
	msg.isCompressed = payloadToSend.isCompressed ? 1 : 0;
	msg.decodedDataSize = htonl(payloadToSend.decodedDataSize);
	msg.encodedDataSize = htonl(encodedDataSize);
	if (!channel.SendTaggedMessage(socket_, "CLIP")) return false;
	if (!channel.SendMessage(socket_, reinterpret_cast<unsigned char*>(& msg), sizeof(msg))) return false;
	if (!payloadToSend.rawData.empty())
		if (!channel.SendMessage(socket_, payloadToSend.rawData.data(), static_cast<uint32_t>(payloadToSend.rawData.size()))) 
			return false;
	return true;
}

void Peer::ThreadProc() {
	while (!stopRequested_.load()) {
		if (!ConnectSocket()) {
			log(__FUNCTION__, Logger::Level::Error, L"Peer: failed to connect.");
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
			log(__FUNCTION__, Logger::Level::Debug, L"Peer: secure handshake failed.");
			CloseSocket();
			if (!stopRequested_.load()) InterruptibleSleep(std::chrono::milliseconds(5000));
			continue;
		}

		log(__FUNCTION__, Logger::Level::Info, L"Peer connected and authenticated.");
		while (!stopRequested_.load()) {
			if (socket_ == INVALID_SOCKET || !channel.SendTaggedMessage(socket_, "PING")) {
				log(__FUNCTION__, Logger::Level::Debug, L"Peer failed secure send");
				break;
			}

			char packet[4] = {};
			if (socket_ == INVALID_SOCKET || !channel.RecvTaggedMessage(socket_, packet)) {
				log(__FUNCTION__, Logger::Level::Debug, L"Peer failed secure recv");
				break;
			}

			if (std::memcmp(packet, "PONG", 4) != 0) {
				log(__FUNCTION__, Logger::Level::Error, L"Peer: unexpected packet received.");
				break;
			}

			{
				std::lock_guard<std::mutex> lock(dataMutex_);
				lastPingReceivedAt_ = std::chrono::steady_clock::now();
			}
			log(__FUNCTION__, Logger::Level::Debug, L"Peer: PONG");

			auto msg = messageQueue_.WaitFor(std::chrono::seconds(30), stopRequested_);

			if (!msg.has_value()) {
				// TIMEOUT or explicit wake: just roll over
			} else {
				// A message was pulled from the queue
				std::shared_ptr<const ClipboardPayload> payload = msg.value();
				log(__FUNCTION__, Logger::Level::Debug, L"Received clipboard payload.");
				if (!SendClipboardData(channel, *payload)) {
					log(__FUNCTION__, Logger::Level::Debug, L"Peer failed to send clipboard payload.");
					break;
				}
			}
		}
		CloseSocket();
		if (!stopRequested_.load()) InterruptibleSleep(std::chrono::milliseconds(5000));
	}
	log(__FUNCTION__, Logger::Level::Info, L"Peer disconnected");
}
