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
#include "utils_socket.h"

Peer::Peer(const wchar_t* hostName, const unsigned char* hostID, const wchar_t* ip, u_short port)
	: hostName_(hostName), ip_(ip), port_(port),
	createdAt_(std::chrono::steady_clock::now()),
	lastPingReceivedAt_(createdAt_) {
	std::memcpy(hostID_.data(), hostID, hostID_.size());
	connType_ = ConnType::Outgoing;
}

Peer::Peer(SOCKET socket, ClipboardReceivedCallback clipboardReceivedCallback)
	: clipboardReceivedCallback_(std::move(clipboardReceivedCallback)),
	ip_(Utf8ToWideString(SocketPeerIp(socket))),
	port_(SocketPeerPort(socket)),
	createdAt_(std::chrono::steady_clock::now()),
	lastPingReceivedAt_(createdAt_),
	socket_(socket) {
	connType_ = ConnType::Incoming;
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
	running_.store(true);
	thread_ = std::thread(connType_ == ConnType::Outgoing ? &Peer::ThreadProcSend : &Peer::ThreadProcRecv, this);
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

bool Peer::isRunning() const {
	return running_.load();
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
	socket_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (socket_ == INVALID_SOCKET) {
		log(__FUNCTION__, Logger::Level::Error, L"Peer: failed to create socket.");
		return false;
	}

	sockaddr_in address{};
	address.sin_family = AF_INET;
	address.sin_port = htons(port());
	std::string peerIp = WideToUtf8String(ip());
	if (inet_pton(AF_INET, peerIp.c_str(), &address.sin_addr) != 1) {
		log(__FUNCTION__, Logger::Level::Error, L"Peer: invalid remote IP address.");
		closesocket(socket_);
		return false;
	}

	if (connect(socket_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR) {
		log(__FUNCTION__, Logger::Level::Debug, L"Peer: TCP connect failed; retrying.");
		closesocket(socket_);
		return false;
	}

	return true;
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

void Peer::ThreadProcSend() {
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

		{
			std::lock_guard<std::mutex> lock(dataMutex_);
			if (hostID_ != remoteHostId) {
				log(__FUNCTION__, Logger::Level::Warning, L"Peer: host ID mismatch; expected %02x... but got %02x...", hostID_[0], remoteHostId[0]);
				if (!stopRequested_.load()) InterruptibleSleep(std::chrono::milliseconds(5000));
				continue;
			}
			if (hostName_ != Utf8ToWideString(remoteHostNameUtf8)) {
				log(__FUNCTION__, Logger::Level::Warning, L"Peer: host name mismatch; expected %ls but got %ls", hostName_.c_str(), Utf8ToWideString(remoteHostNameUtf8).c_str());
				if (!stopRequested_.load()) InterruptibleSleep(std::chrono::milliseconds(5000));
				continue;
			}
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
				log(__FUNCTION__, Logger::Level::Debug, L"Clipboard payload to be sent: format ID %u, encoded size %zu bytes, decoded size %u bytes", payload->formatId, payload->rawData.size(), payload->decodedDataSize);
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
	running_.store(false);
}

void Peer::ThreadProcRecv() {
	CryptoChannel channel;
	std::array<unsigned char, 32> remoteHostId{};
	std::string remoteHostNameUtf8;
	if (!channel.ServerHandshake(socket_, remoteHostId, remoteHostNameUtf8)) {
		log(__FUNCTION__, Logger::Level::Error, L"Client secure handshake failed.");
	}
	else {
		{
			std::lock_guard<std::mutex> lock(dataMutex_);
			hostID_ = remoteHostId;
			hostName_ = Utf8ToWideString(remoteHostNameUtf8);
		}
		log(__FUNCTION__, Logger::Level::Info, L"Client connected");

		char packet[4] = {};
		while (!stopRequested_.load()) {
			if (!channel.RecvTaggedMessage(socket_, packet)) {
				break;
			}

			if (std::memcmp(packet, "PING", 4) == 0) {
				log(__FUNCTION__, Logger::Level::Debug, L"Client: PING");
				{
					std::lock_guard<std::mutex> lock(dataMutex_);
					lastPingReceivedAt_ = std::chrono::steady_clock::now();
				}
				if (!channel.SendTaggedMessage(socket_, "PONG")) {
					break;
				}
				continue;
			}

			if (std::memcmp(packet, "CLIP", 4) == 0) {
				std::vector<unsigned char> headerMsg;
				if (!channel.RecvMessage(socket_, headerMsg) || headerMsg.size() != sizeof(NetworkDefs::ClipboardMessage)) {
					break;
				}
				if (headerMsg.size() != sizeof(NetworkDefs::ClipboardMessage)) {
					log(__FUNCTION__, Logger::Level::Warning, L"Rejecting clipboard message: header size mismatch (expected %zu bytes, actual %zu bytes)", sizeof(NetworkDefs::ClipboardMessage), headerMsg.size());
					break;
				}

				auto* clipMessage = reinterpret_cast<NetworkDefs::ClipboardMessage*>(headerMsg.data());
				ClipboardPayload payload{};
				payload.formatId = ntohl(clipMessage->formatId);
				payload.decodedDataSize = ntohl(clipMessage->decodedDataSize);
				payload.isCompressed = clipMessage->isCompressed != 0;
				uint32_t encodedDataSize = ntohl(clipMessage->encodedDataSize);

				if (!channel.RecvMessage(socket_, payload.rawData)) {
					break;
				}

				if (payload.rawData.size() != static_cast<size_t>(encodedDataSize)) {
					log(__FUNCTION__, Logger::Level::Warning, L"Rejecting clipboard message: encoded size mismatch (header: %u bytes, body: %zu bytes)", encodedDataSize, payload.rawData.size());
					break;
				}

				if (payload.decodedDataSize > ClipboardLimits::kMaxDecompressedClipboardBytes) {
					log(__FUNCTION__, Logger::Level::Warning, L"Rejecting clipboard message: decoded size %u bytes exceeds limit %llu bytes", payload.decodedDataSize, ClipboardLimits::kMaxDecompressedClipboardBytes);
					break;
				}

				if (!payload.isCompressed && encodedDataSize != payload.decodedDataSize) {
					log(__FUNCTION__, Logger::Level::Warning, L"Rejecting uncompressed clipboard message: encoded size %u bytes does not equal decoded size %u bytes", encodedDataSize, payload.decodedDataSize);
					break;
				}

				if (!payload.ZstdDecompress()) {
					break;
				}

				if (clipboardReceivedCallback_) {
					std::array<unsigned char, 32> remoteHostId;
					std::wstring remoteHostName;
					{
						std::lock_guard<std::mutex> lock(dataMutex_);
						remoteHostId = hostID_;
						remoteHostName = hostName_;
					}
					clipboardReceivedCallback_(remoteHostName, remoteHostId, payload);
				}
			}
		}
		log(__FUNCTION__, Logger::Level::Info, L"Client disconnected");
	}

	running_.store(false);
}
