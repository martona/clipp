#include "Logger.h"
#include "Peer.h"

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <limits>
#include <thread>
#include <utility>

#include "Settings.h"
#include "CryptoChannel.h"
#include "NetworkDefs.h"
#include "utils.h"
#include "utils_socket.h"
#include "PeerManager.h"

#if defined(__APPLE__)
#include <fcntl.h>
#include <sys/select.h>
#endif

extern PeerManager g_peerManager;

static bool SetSocketBlockingMode(SOCKET socket, bool blocking) {
#ifdef _WIN32
	u_long mode = blocking ? 0 : 1;
	return ioctlsocket(socket, FIONBIO, &mode) == 0;
#else
	int flags = fcntl(socket, F_GETFL, 0);
	if (flags == -1) return false;
	flags = blocking ? (flags & ~O_NONBLOCK) : (flags | O_NONBLOCK);
	return fcntl(socket, F_SETFL, flags) == 0;
#endif
}

static int GetLastSocketError() {
#ifdef _WIN32
	return WSAGetLastError();
#else
	return errno;
#endif
}

static bool IsConnectPendingError(int error) {
#ifdef _WIN32
	return error == WSAEWOULDBLOCK || error == WSAEINPROGRESS || error == WSAEALREADY;
#else
	return error == EINPROGRESS || error == EWOULDBLOCK || error == EALREADY;
#endif
}

static bool GetPendingConnectError(SOCKET socket, int& connectError) {
	socklen_t optionLength = sizeof(connectError);
	connectError = 0;
	return getsockopt(socket, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&connectError), &optionLength) == 0;
}

Peer::Peer(const wchar_t* hostName, const unsigned char* hostID, const wchar_t* ip, u_short port, VerifiedCallback verifiedCallback, TrafficCallback trafficCallback)
	: hostName_(hostName), ip_(ip), port_(port),
	createdAt_(std::chrono::steady_clock::now()),
	lastPingReceivedAt_(createdAt_),
	verifiedCallback_(std::move(verifiedCallback)),
	trafficCallback_(std::move(trafficCallback)) {
	std::memcpy(hostID_.data(), hostID, hostID_.size());
	connType_ = ConnType::Outgoing;
}

Peer::Peer(SOCKET socket, ClipboardReceivedCallback clipboardReceivedCallback, VerifiedCallback verifiedCallback, TrafficCallback trafficCallback)
	: ip_(Utf8ToWideString(SocketPeerIp(socket))),
	port_(SocketPeerPort(socket)),
	createdAt_(std::chrono::steady_clock::now()),
	lastPingReceivedAt_(createdAt_),
	clipboardReceivedCallback_(std::move(clipboardReceivedCallback)),
	verifiedCallback_(std::move(verifiedCallback)),
	trafficCallback_(std::move(trafficCallback)),
	socket_(socket) {
	connType_ = ConnType::Incoming;
}

void Peer::log(const char* function, Logger::Level level, const wchar_t* message, ...) const {
	va_list args;
	va_start(args, message);
	logV(function, level, message, args);
	va_end(args);
}

const wchar_t* Peer::ConnTypeString() const {
	switch (connType_) {
	case ConnType::Outgoing:
		return L"Out";
	case ConnType::Incoming:
		return L"Inc";
	default:
		return L"Unk";
	}
}

void Peer::logV(const char* function, Logger::Level level, const wchar_t* message, va_list args) const {
	wchar_t formattedMessage[1024];
	int bufferSize = cntof(formattedMessage);
	{
		std::lock_guard<std::mutex> lock(dataMutex_);
		int prefixlen = snwprintf_truncate(formattedMessage, bufferSize, L"[%ls %ls %ls] ",
			hostName_.empty() ? L"<unknown>" : hostName_.c_str(),
			ip_.empty() ? L"<unknown>" : ip_.c_str(),
			ConnTypeString());
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
	ShutdownSocket();
	if (thread_.joinable()) {
		thread_.join();
	}
	CloseSocket();
}

void Peer::InterruptibleSleep(std::chrono::milliseconds duration) {
	std::unique_lock<std::mutex> lock(stopMutex_);
	stopCV_.wait_for(lock, duration, [this]() { return stopRequested_.load(); });
}

bool Peer::isRunning() const {
	return running_.load();
}

bool Peer::isDisplayRegistered() const {
	return displayRegistered_.load();
}

void Peer::MarkDisplayRegistered() {
	displayRegistered_.store(true);
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

void Peer::ReportTraffic(uint64_t bytesSent, uint64_t bytesReceived) {
	if (!trafficCallback_ || (bytesSent == 0 && bytesReceived == 0)) return;

	std::wstring currentHostName;
	std::array<unsigned char, 32> currentHostID{};
	{
		std::lock_guard<std::mutex> lock(dataMutex_);
		currentHostName = hostName_;
		currentHostID = hostID_;
	}
	trafficCallback_(currentHostName, currentHostID, bytesSent, bytesReceived);
}

SOCKET Peer::CurrentSocket() const {
	std::lock_guard<std::mutex> lock(socketMutex_);
	return socket_;
}

void Peer::SetSocket(SOCKET socket) {
	std::lock_guard<std::mutex> lock(socketMutex_);
	socket_ = socket;
}

SOCKET Peer::DetachSocket() {
	std::lock_guard<std::mutex> lock(socketMutex_);
	SOCKET socket = socket_;
	socket_ = INVALID_SOCKET;
	return socket;
}

void Peer::ShutdownSocket() {
	SOCKET socket = CurrentSocket();
	if (socket != INVALID_SOCKET) {
		shutdown(socket, SD_BOTH);
	}
}

void Peer::CloseSocket() {
	SOCKET socket = DetachSocket();
	if (socket != INVALID_SOCKET) {
		shutdown(socket, SD_BOTH);
		closesocket(socket);
	}
}

bool Peer::ConnectSocket() {
	SOCKET socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (socket == INVALID_SOCKET) {
		log(__FUNCTION__, Logger::Level::Error, L"Peer: failed to create socket.");
		return false;
	}
	SetSocket(socket);

	sockaddr_in address{};
	address.sin_family = AF_INET;
	address.sin_port = htons(port());
	std::string peerIp = WideToUtf8String(ip());
	if (inet_pton(AF_INET, peerIp.c_str(), &address.sin_addr) != 1) {
		log(__FUNCTION__, Logger::Level::Error, L"Peer: invalid remote IP address.");
		CloseSocket();
		return false;
	}

	if (!SetSocketBlockingMode(socket, false)) {
		log(__FUNCTION__, Logger::Level::Error, L"Peer: failed to set socket nonblocking.");
		CloseSocket();
		return false;
	}

	if (connect(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR) {
		const int connectStartError = GetLastSocketError();
		if (!IsConnectPendingError(connectStartError)) {
			log(__FUNCTION__, Logger::Level::Debug, L"Peer: TCP connect failed; retrying.");
			CloseSocket();
			return false;
		}

		bool connected = false;
		while (!stopRequested_.load()) {
			fd_set writeSet;
			fd_set errorSet;
			FD_ZERO(&writeSet);
			FD_ZERO(&errorSet);
			FD_SET(socket, &writeSet);
			FD_SET(socket, &errorSet);

			timeval timeout{};
			timeout.tv_usec = 250 * 1000;

			const int selected = select(static_cast<int>(socket) + 1, nullptr, &writeSet, &errorSet, &timeout);
			if (selected == SOCKET_ERROR) {
				log(__FUNCTION__, Logger::Level::Debug, L"Peer: TCP connect poll failed; retrying.");
				CloseSocket();
				return false;
			}
			if (selected == 0) {
				continue;
			}

			int connectError = 0;
			if (!GetPendingConnectError(socket, connectError) || connectError != 0) {
				log(__FUNCTION__, Logger::Level::Debug, L"Peer: TCP connect failed; retrying.");
				CloseSocket();
				return false;
			}

			connected = true;
			break;
		}

		if (!connected) {
			CloseSocket();
			return false;
		}
	}

	if (!SetSocketBlockingMode(socket, true)) {
		log(__FUNCTION__, Logger::Level::Error, L"Peer: failed to restore socket blocking mode.");
		CloseSocket();
		return false;
	}

	return true;
}

bool Peer::SendClipboardData(CryptoChannel& channel, SOCKET socket, const ClipboardPayload& payload) {
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
	if (socket == INVALID_SOCKET || !channel.SendTaggedMessage(socket, "CLIP")) return false;
	if (!channel.SendMessage(socket, reinterpret_cast<unsigned char*>(& msg), sizeof(msg))) return false;
	if (!payloadToSend.rawData.empty())
		if (!channel.SendMessage(socket, payloadToSend.rawData.data(), static_cast<uint32_t>(payloadToSend.rawData.size())))
			return false;
	ReportTraffic(4 + sizeof(NetworkDefs::ClipboardMessage) + payloadToSend.rawData.size(), 0);
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
		SOCKET socket = CurrentSocket();
		if (socket == INVALID_SOCKET || !channel.ClientHandshake(socket, localHostId, localHostNameA, remoteHostId, remoteHostNameUtf8)) {
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
			if (socket == INVALID_SOCKET || !channel.SendTaggedMessage(socket, "PING")) {
				log(__FUNCTION__, Logger::Level::Debug, L"Peer failed secure send");
				break;
			}
			ReportTraffic(4, 0);
			log(__FUNCTION__, Logger::Level::Debug, L"PING?");

			char packet[4] = {};
			if (socket == INVALID_SOCKET || !channel.RecvTaggedMessage(socket, packet)) {
				log(__FUNCTION__, Logger::Level::Debug, L"Peer failed secure recv");
				break;
			}

			ReportTraffic(0, 4);

			if (std::memcmp(packet, "PONG", 4) != 0) {
				log(__FUNCTION__, Logger::Level::Error, L"Peer: unexpected packet received.");
				break;
			}

			{
				std::lock_guard<std::mutex> lock(dataMutex_);
				lastPingReceivedAt_ = std::chrono::steady_clock::now();
			}
			log(__FUNCTION__, Logger::Level::Debug, L"PONG");

			auto msg = messageQueue_.WaitFor(std::chrono::seconds(30), stopRequested_);

			if (!msg.has_value()) {
				// TIMEOUT or explicit wake: just roll over
			} else {
				// A message was pulled from the queue
				std::shared_ptr<const ClipboardPayload> payload = msg.value();
				log(__FUNCTION__, Logger::Level::Debug, L"Clipboard payload to be sent: format ID %u, encoded size %zu bytes, decoded size %u bytes", payload->formatId, payload->rawData.size(), payload->decodedDataSize);
				if (!SendClipboardData(channel, socket, *payload)) {
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
	log(__FUNCTION__, Logger::Level::Info, L"Thread exiting");
}

void Peer::ThreadProcRecv() {
	CryptoChannel channel;
	std::array<unsigned char, 32> remoteHostId{};
	std::string remoteHostNameUtf8;
	SOCKET socket = CurrentSocket();
	if (socket == INVALID_SOCKET || !channel.ServerHandshake(socket, remoteHostId, remoteHostNameUtf8)) {
		log(__FUNCTION__, Logger::Level::Error, L"Client secure handshake failed.");
	}
	else {
		{
			std::lock_guard<std::mutex> lock(dataMutex_);
			hostID_ = remoteHostId;
			hostName_ = Utf8ToWideString(remoteHostNameUtf8);
		}
		if (verifiedCallback_) {
			verifiedCallback_(Utf8ToWideString(remoteHostNameUtf8), remoteHostId, connType_, createdAt_);
			displayRegistered_.store(true);
		}

		log(__FUNCTION__, Logger::Level::Info, L"Client connected");

		char packet[4] = {};
		while (!stopRequested_.load()) {
			if (socket == INVALID_SOCKET || !channel.RecvTaggedMessage(socket, packet)) {
				break;
			}
			ReportTraffic(0, 4);

			if (std::memcmp(packet, "PING", 4) == 0) {
				log(__FUNCTION__, Logger::Level::Debug, L"PING");
				{
					std::lock_guard<std::mutex> lock(dataMutex_);
					lastPingReceivedAt_ = std::chrono::steady_clock::now();
				}
				if (!channel.SendTaggedMessage(socket, "PONG")) {
					break;
				}
				log(__FUNCTION__, Logger::Level::Debug, L"PONG!");
				ReportTraffic(4, 0);
				continue;
			}

			if (std::memcmp(packet, "CLIP", 4) == 0) {
				std::vector<unsigned char> headerMsg;
				if (!channel.RecvMessage(socket, headerMsg) || headerMsg.size() != sizeof(NetworkDefs::ClipboardMessage)) {
					break;
				}
				ReportTraffic(0, headerMsg.size());
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

				if (!channel.RecvMessage(socket, payload.rawData)) {
					break;
				}
				ReportTraffic(0, payload.rawData.size());

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

	CloseSocket();
	running_.store(false);
	std::thread([]() {
			g_logger.log(__FUNCTION__, Logger::Level::Debug, L"Lambda culling...");
			g_peerManager.CullPeers();
			g_logger.log(__FUNCTION__, Logger::Level::Debug, L"Lambda culled");
		}).detach();
	log(__FUNCTION__, Logger::Level::Info, L"Thread exiting");
}
