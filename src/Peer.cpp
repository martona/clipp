#include "Logger.h"
#include "Peer.h"

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <limits>
#include <optional>
#include <thread>
#include <utility>
#include <variant>

#include "Settings.h"
#include "ClipboardActivityStore.h"
#include "ClipboardWire.h"
#include "CryptoChannel.h"
#include "LocalPeerName.h"
#include "NetworkDefs.h"
#include "PeerDisplay.h"
#include "utils.h"
#include "utils_socket.h"
#include "PeerManager.h"
#include "RegisterConfig.h"
#include "RegisterStore.h"
#include "RegisterWire.h"

#if CLIPP_REGISTERS_DAEMON
// Generated into the CMake build tree; only the desktop daemon builds have it on
// the include path (iOS compiles this file via the .mm bridge with no generated
// headers). Sole consumer is the NMAP responder's self line, same gate.
#include "version.h"
#endif

#if defined(__APPLE__)
#include <fcntl.h>
#include <sys/select.h>
#endif

extern PeerManager g_peerManager;
extern PeerDisplay g_peerDisplay;

static std::atomic<uint64_t> g_nextPeerLogId{ 1 };

// Outgoing reconnect backoff schedule. Capped at 60s; we never give up — discovery
// being event-driven means we should keep retrying until the OS reports the peer gone.
static const std::array<std::chrono::seconds, 6> g_outgoingBackoff = {
	std::chrono::seconds(1),
	std::chrono::seconds(2),
	std::chrono::seconds(5),
	std::chrono::seconds(15),
	std::chrono::seconds(60),
	std::chrono::seconds(60),
};

static void CullStoppedPeersAsync() {
	std::thread([]() {
			g_peerManager.CullPeers();
		}).detach();
}

static std::chrono::milliseconds NextPingInterval() {
	constexpr auto baseInterval = std::chrono::seconds(30);
	const auto jitter = std::chrono::milliseconds(randombytes_uniform(5001));
	return std::chrono::duration_cast<std::chrono::milliseconds>(baseInterval) + jitter;
}

Peer::Peer(const wchar_t* hostName, const HostId* hostID, const wchar_t* ip, u_short port, VerifiedCallback verifiedCallback, TrafficCallback trafficCallback)
	: logId_(g_nextPeerLogId.fetch_add(1)), hostName_(hostName), ip_(ip), port_(port),
	hostID_(*hostID),
	createdAt_(std::chrono::steady_clock::now()),
	lastPingReceivedAt_(createdAt_),
	verifiedCallback_(std::move(verifiedCallback)),
	trafficCallback_(std::move(trafficCallback)) {
	connType_ = ConnType::Outgoing;
}

Peer::Peer(SOCKET socket, ClipboardReceivedCallback clipboardReceivedCallback, VerifiedCallback verifiedCallback, TrafficCallback trafficCallback)
	: logId_(g_nextPeerLogId.fetch_add(1)),
	ip_(Utf8ToWideString(SocketPeerIp(socket))),
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
		int prefixlen = snwprintf_truncate(formattedMessage, bufferSize, L"[#%llu %ls %ls %ls] ",
			static_cast<unsigned long long>(logId_),
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
	if (!wakeEvent_.Initialize()) {
		log(__FUNCTION__, Logger::Level::Error, L"Peer: failed to create wake socket.");
		CloseSocket();
		running_.store(false);
		CullStoppedPeersAsync();
		return;
	}
	stopRequested_.store(false);
	running_.store(true);
	thread_ = std::thread(connType_ == ConnType::Outgoing ? &Peer::ThreadProcSend : &Peer::ThreadProcRecv, this);
}

void Peer::Stop() {
	log(__FUNCTION__, Logger::Level::Debug, L"Stopping Peer.");
	stopRequested_.store(true);
	stopCV_.notify_all();
	messageQueue_.WakeAll();
	wakeEvent_.Signal();
	if (thread_.joinable()) {
		thread_.join();
	}
	CloseSocket();
	wakeEvent_.Close();
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

HostId Peer::hostID() const {
	std::lock_guard<std::mutex> lock(dataMutex_);
	return hostID_;
}

OsType Peer::osType() const {
	std::lock_guard<std::mutex> lock(dataMutex_);
	return osType_;
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

void Peer::PushMessage(std::shared_ptr<const ClipboardPayload> payload) {
	messageQueue_.Push(OutboundMessage{ std::move(payload) });
	wakeEvent_.Signal();
}

void Peer::PushRawFrame(std::array<char, 4> tag, std::vector<unsigned char> body) {
	messageQueue_.Push(OutboundMessage{ OutboundRawFrame{ tag, std::move(body) } });
	wakeEvent_.Signal();
}

void Peer::ReportTraffic(uint64_t bytesSent, uint64_t bytesReceived) {
	if (!trafficCallback_ || (bytesSent == 0 && bytesReceived == 0)) return;

	HostId currentHostID;
	{
		std::lock_guard<std::mutex> lock(dataMutex_);
		currentHostID = hostID_;
	}
	trafficCallback_(currentHostID, bytesSent, bytesReceived);
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
	ConfigureTcpSocket(socket);
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
		const int connectStartError = LastSocketError();
		if (!IsConnectPendingError(connectStartError)) {
			log(__FUNCTION__, Logger::Level::Debug, L"Peer: TCP connect failed; retrying.");
			CloseSocket();
			return false;
		}

		bool connected = false;
		while (!stopRequested_.load()) {
			fd_set readSet;
			fd_set writeSet;
			fd_set errorSet;
			FD_ZERO(&readSet);
			FD_ZERO(&writeSet);
			FD_ZERO(&errorSet);
			FD_SET(wakeEvent_.Socket(), &readSet);
			FD_SET(socket, &writeSet);
			FD_SET(socket, &errorSet);

			const SOCKET maxSock = (std::max)(socket, wakeEvent_.Socket());
			const int selected = select(static_cast<int>(maxSock) + 1, &readSet, &writeSet, &errorSet, nullptr);
			if (selected == SOCKET_ERROR) {
				log(__FUNCTION__, Logger::Level::Debug, L"Peer: TCP connect poll failed; retrying.");
				CloseSocket();
				return false;
			}
			if (FD_ISSET(wakeEvent_.Socket(), &readSet)) {
				wakeEvent_.Drain();
				CloseSocket();
				return false;
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

	return true;
}

bool Peer::SendClipboardData(CryptoChannel& channel, const SocketIoContext& io, const ClipboardPayload& payload) {
	size_t bytesSent = 0;
	if (!ClipboardWire::SendClipboardPayload(channel, io, payload, &bytesSent)) return false;
	ReportTraffic(bytesSent, 0);
	return true;
}

bool Peer::DrainOutboundMessages(CryptoChannel& channel, const SocketIoContext& io) {
	for (;;) {
		std::optional<OutboundMessage> msg = messageQueue_.TryPop();
		if (!msg.has_value()) {
			return true;
		}

		if (const auto* payloadPtr = std::get_if<std::shared_ptr<const ClipboardPayload>>(&msg.value())) {
			const std::shared_ptr<const ClipboardPayload>& payload = *payloadPtr;
			log(__FUNCTION__, Logger::Level::Debug, L"Clipboard payload to be sent: format %ls (%u), payload size %zu bytes, uncompressed size %llu bytes",
				ClippClipboardFormatNameW(payload->meta.formatId),
				payload->meta.formatId,
				payload->EncodedBytes().size(),
				static_cast<unsigned long long>(payload->meta.uncompressedDataSize));
			if (!SendClipboardData(channel, io, *payload)) {
				log(__FUNCTION__, Logger::Level::Debug, L"Peer failed to send clipboard payload.");
				return false;
			}
		} else {
			const OutboundRawFrame& raw = std::get<OutboundRawFrame>(msg.value());
			if (!channel.SendFrame(io, raw.tag.data(),
					raw.body.empty() ? nullptr : raw.body.data(),
					static_cast<uint32_t>(raw.body.size()))) {
				log(__FUNCTION__, Logger::Level::Debug, L"Peer failed to send raw frame '%c%c%c%c'.",
					static_cast<wchar_t>(raw.tag[0]), static_cast<wchar_t>(raw.tag[1]),
					static_cast<wchar_t>(raw.tag[2]), static_cast<wchar_t>(raw.tag[3]));
				return false;
			}
			ReportTraffic(4 + raw.body.size() + crypto_secretstream_xchacha20poly1305_ABYTES + sizeof(uint32_t), 0);
		}
	}
}

bool Peer::HandleRegisterFrame(CryptoChannel& channel, const SocketIoContext& io, const std::vector<unsigned char>& frame) {
#if CLIPP_REGISTERS_DAEMON
	// frame = 4-byte tag + body; slice the body once for the decoders.
	const std::vector<unsigned char> body(frame.begin() + 4, frame.end());

	if (std::memcmp(frame.data(), "REGW", 4) == 0) {
		RegisterRecord rec;
		uint8_t transport = 0;
		if (!RegisterWire::TryDecodeRecord(body, rec, transport) || !IsValidRegisterName(rec.name)) {
			log(__FUNCTION__, Logger::Level::Warning, L"Peer: malformed or invalid REGW; ignoring.");
			return true;
		}
		if ((transport & RegisterWire::Transport::Relay) != 0) {
			// Origin write request from a one-shot client: re-stamp with our
			// authoritative clock (keeping the client's origin) and ACK on the same
			// connection, so the client's socket stays open until the write has
			// actually landed; then rebroadcast the result to the mesh.
			const std::string name = rec.name;
			bool changed = false;
			if (rec.IsTombstone()) {
				const auto result = g_registerStore.Delete(name, rec.originHostId);
				changed = (result == RegisterStore::DeleteResult::Deleted);
				channel.SendFrame(io, changed ? "RDEL" : "NONE");
			} else {
				const auto result = g_registerStore.Upsert(name, std::move(rec.value), rec.IsPrivate(), rec.originHostId);
				changed = (result == RegisterStore::WriteResult::Ok);
				channel.SendFrame(io, changed ? "ROKW" : "RERR");
			}
			if (changed) {
				g_settings.noteRegisterHlcWallMs(g_registerStore.ClockHighWater().wallMs);
				if (const auto stored = g_registerStore.GetForBroadcast(name)) {
					const auto regwOut = RegisterWire::EncodeRecord(*stored, 0);
					g_peerManager.BroadcastRegisterFrame({ 'R', 'E', 'G', 'W' }, regwOut);
				}
			}
		} else {
			g_registerStore.ApplyRemote(std::move(rec));
		}
		return true;
	}
	if (std::memcmp(frame.data(), "RSYN", 4) == 0) {
		std::vector<RegisterDigestEntry> theirDigest;
		if (!RegisterWire::TryDecodeDigest(body, theirDigest)) {
			log(__FUNCTION__, Logger::Level::Warning, L"Peer: malformed RSYN; ignoring.");
			return true;
		}
		const auto toPush = g_registerStore.RecordsToPush(theirDigest);
		for (const auto& rec : toPush) {
			const auto regw = RegisterWire::EncodeRecord(rec, 0);
			if (!channel.SendFrame(io, "REGW", regw.data(), static_cast<uint32_t>(regw.size()))) {
				log(__FUNCTION__, Logger::Level::Debug, L"Peer: REGW send failed during anti-entropy.");
				return true;
			}
		}
		channel.SendFrame(io, "REOS");
		log(__FUNCTION__, Logger::Level::Debug, L"Answered RSYN: pushed %zu register record(s).", toPush.size());
		return true;
	}
	if (std::memcmp(frame.data(), "REOS", 4) == 0) {
		log(__FUNCTION__, Logger::Level::Debug, L"Register anti-entropy from this peer complete.");
		return true;
	}
	if (std::memcmp(frame.data(), "RGET", 4) == 0) {
		std::string name;
		std::optional<RegisterRecord> rec;
		if (RegisterWire::TryDecodeName(body, name) && IsValidRegisterName(name)) {
			rec = g_registerStore.Read(name);  // touch side effect at the serving peer
		}
		if (rec.has_value()) {
			const auto regw = RegisterWire::EncodeRecord(*rec, 0);
			channel.SendFrame(io, "REGW", regw.data(), static_cast<uint32_t>(regw.size()));
		} else {
			channel.SendFrame(io, "NONE");
		}
		return true;
	}
	if (std::memcmp(frame.data(), "RPUT", 4) == 0) {
		// `clipp put <name>`: promote a named register to the live clipboard as one
		// atomic gateway-side op (the CLI's fallback is RGET -> CLIP relay). Reuses
		// the relay intake: flag the item CLPM_FLAG_RELAY and hand it to the
		// clipboard callback, which rebroadcasts to the mesh (flag cleared), applies
		// it locally, and mirrors the "" register. ROKP is sent after that returns,
		// so the ack means "applied here and queued to every peer"; NONE = no such
		// register; RERR = couldn't build or route the item.
		std::string name;
		std::optional<RegisterRecord> rec;
		if (RegisterWire::TryDecodeName(body, name) && IsValidRegisterName(name)) {
			rec = g_registerStore.Read(name);  // touch side effect, like RGET
		}
		if (!rec.has_value()) {
			channel.SendFrame(io, "NONE");
			return true;
		}
		// The same item `clipp copy` would send: canonical-LF text plus the
		// capture-convention trailing NUL. Origin = the requesting device (the put
		// is user-initiated there); the sequence number is diagnostic and comes
		// from our counter.
		std::vector<unsigned char> bytes(rec->value.begin(), rec->value.end());
		bytes.push_back('\0');
		ClipboardPayload payload;
		payload.meta.formatId = CLIPP_FORMAT_UTF8;
		bool routed = false;
		if (payload.SetUncompressedBytes(std::move(bytes))) {
			payload.StampOrigin(hostID(), WideToUtf8String(hostName()).c_str(),
				g_settings.nextOriginSequenceNumber());
			payload.meta.flags |= NetworkDefs::CLPM_FLAG_RELAY;
			if (clipboardReceivedCallback_) {
				clipboardReceivedCallback_(std::make_shared<const ClipboardPayload>(std::move(payload)));
				routed = true;
			}
		}
		channel.SendFrame(io, routed ? "ROKP" : "RERR");
		return true;
	}
	if (std::memcmp(frame.data(), "RLST", 4) == 0) {
		// Rich list for `ls`/`ls -v`: live values + the default "" mirror, each with a
		// capped preview (omitted for private records — their bytes never leave the
		// gateway for a listing).
		// The CLI has no hostId->name map, so resolve each origin to a device name here
		// (the daemon does have one) and ship it in the entry. Snapshot the peer list
		// once; ids we can't place stay "" and the CLI shows the short id instead.
		HostId localHostId{};
		const bool haveLocalHostId = g_settings.getHostID(localHostId);
		const std::string localHostName = clipp::GetLocalPeerDisplayName("", CryptoChannel::HOSTNAME_MAX_BYTES);
		const auto knownPeers = g_peerDisplay.Query();
		const auto resolveOriginName = [&](const HostId& origin) -> std::string {
			if (haveLocalHostId && origin == localHostId) return localHostName;
			for (const auto& peer : knownPeers) {
				if (peer.hostID == origin) return WideToUtf8String(peer.hostName);
			}
			return {};
		};
		std::vector<RegisterWire::RegisterListEntry> list;
		for (const auto& storeRec : g_registerStore.List()) {
			RegisterWire::RegisterListEntry e;
			e.name = storeRec.name;
			e.touched = storeRec.touched;
			e.valueSize = storeRec.value.size();
			e.originHostId = storeRec.originHostId;
			e.originHostName = resolveOriginName(storeRec.originHostId);
			e.flags = storeRec.flags;
			if (!storeRec.IsPrivate()) {
				const size_t n = storeRec.value.size() < RegisterWire::kMaxPreviewLen
					? storeRec.value.size() : RegisterWire::kMaxPreviewLen;
				e.preview.assign(storeRec.value.data(), storeRec.value.data() + n);
			}
			list.push_back(std::move(e));
		}
		const auto listBody = RegisterWire::EncodeList(list);
		channel.SendFrame(io, "RLST", listBody.data(), static_cast<uint32_t>(listBody.size()));
		return true;
	}
	return false;
#else
	(void)channel;
	(void)io;
	(void)frame;
	return false;
#endif
}

void Peer::SendRegisterSyncOnConnect(CryptoChannel& channel, const SocketIoContext& io) {
#if CLIPP_REGISTERS_DAEMON
	const auto digest = g_registerStore.Digest();
	const auto body = RegisterWire::EncodeDigest(digest);
	if (channel.SendFrame(io, "RSYN", body.data(), static_cast<uint32_t>(body.size()))) {
		log(__FUNCTION__, Logger::Level::Debug, L"Sent register digest (%zu entries) for anti-entropy.", digest.size());
	} else {
		log(__FUNCTION__, Logger::Level::Debug, L"Peer: failed to send register RSYN.");
	}
#else
	(void)channel;
	(void)io;
#endif
}

void Peer::ThreadProcSend() {
	size_t backoffIdx = 0;
	auto goBackoff = [&]() {
		const auto delay = g_outgoingBackoff[(std::min)(backoffIdx, g_outgoingBackoff.size() - 1)];
		++backoffIdx;
		g_peerDisplay.NotifyPeerConnState(hostName(), hostID(), PeerConnState::Backoff);
		log(__FUNCTION__, Logger::Level::Debug, L"Peer: backing off %lld s before reconnect.",
			static_cast<long long>(delay.count()));
		InterruptibleSleep(std::chrono::duration_cast<std::chrono::milliseconds>(delay));
	};

	while (!stopRequested_.load()) {
		g_peerDisplay.NotifyPeerConnState(hostName(), hostID(), PeerConnState::Connecting);

		if (!ConnectSocket()) {
			if (stopRequested_.load()) break;
			goBackoff();
			continue;
		}

		CryptoChannel channel;
		HostId remoteHostId;
		std::string remoteHostNameUtf8;
		HostId localHostId;
		if (!g_settings.getHostID(localHostId)) {
			CloseSocket();
			break;
		}
		const std::string localHostName = clipp::GetLocalPeerDisplayName("unknown", CryptoChannel::HOSTNAME_MAX_BYTES);
		SOCKET socket = CurrentSocket();
		const SocketIoContext io{ socket, wakeEvent_, stopRequested_ };
		if (socket == INVALID_SOCKET || !channel.ClientHandshake(io, localHostId, localHostName, remoteHostId, remoteHostNameUtf8)) {
			log(__FUNCTION__, Logger::Level::Debug, L"Peer: secure handshake failed.");
			CloseSocket();
			if (stopRequested_.load()) break;
			goBackoff();
			continue;
		}

		const std::wstring remoteHostName = Utf8ToWideString(remoteHostNameUtf8);
		bool hostIDMismatch = false;
		bool hostNameMismatch = false;
		std::wstring expectedHostName;
		{
			std::lock_guard<std::mutex> lock(dataMutex_);
			hostIDMismatch = hostID_ != remoteHostId;
			hostNameMismatch = hostName_ != remoteHostName;
			expectedHostName = hostName_;
			osType_ = channel.RemoteOsType();
			remoteServesRegisters_.store((channel.RemoteCaps()[0] & CryptoChannel::CAP0_SERVES_REGISTERS) != 0);
		}
		if (hostIDMismatch) {
			log(__FUNCTION__, Logger::Level::Warning, L"Peer: host ID mismatch");
			CloseSocket();
			// Identity mismatch won't fix itself; back off and retry — discovery may correct it.
			if (stopRequested_.load()) break;
			goBackoff();
			continue;
		}
		if (hostNameMismatch) {
			log(__FUNCTION__, Logger::Level::Warning, L"Peer: host name mismatch; expected %ls but got %ls", expectedHostName.c_str(), remoteHostName.c_str());
			// Tolerate name drift — accept the connection. Update tracked name.
			std::lock_guard<std::mutex> lock(dataMutex_);
			hostName_ = remoteHostName;
		}

		log(__FUNCTION__, Logger::Level::Info, L"Peer connected and authenticated.");
		backoffIdx = 0;
		g_peerDisplay.NotifyPeerConnState(hostName(), hostID(), PeerConnState::Connected);
		// Register anti-entropy: offer our digest; the peer pushes back what we lack.
		SendRegisterSyncOnConnect(channel, io);
		auto nextPingTime = std::chrono::steady_clock::now();
		std::vector<unsigned char> frame;
		while (!stopRequested_.load()) {
			const auto now = std::chrono::steady_clock::now();
			if (now >= nextPingTime) {
				if (io.socket == INVALID_SOCKET || !channel.SendFrame(io, "PING")) {
					log(__FUNCTION__, Logger::Level::Debug, L"Peer failed secure send");
					break;
				}
				// 4-byte tag + AEAD tag + length prefix; matches what the recv side counts.
				ReportTraffic(4 + crypto_secretstream_xchacha20poly1305_ABYTES + sizeof(uint32_t), 0);
				log(__FUNCTION__, Logger::Level::DDebug, L"PING?");
				nextPingTime = now + NextPingInterval();
			}

			if (!DrainOutboundMessages(channel, io)) {
				break;
			}

			fd_set readSet;
			FD_ZERO(&readSet);
			FD_SET(socket, &readSet);
			FD_SET(wakeEvent_.Socket(), &readSet);

			timeval timeout{};
			const auto timeoutDuration = nextPingTime - std::chrono::steady_clock::now();
			if (timeoutDuration > std::chrono::steady_clock::duration::zero()) {
				const auto timeoutMs = std::chrono::duration_cast<std::chrono::milliseconds>(timeoutDuration);
				timeout.tv_sec = static_cast<long>(timeoutMs.count() / 1000);
					timeout.tv_usec = static_cast<decltype(timeout.tv_usec)>((timeoutMs.count() % 1000) * 1000);
				}

			const SOCKET maxSock = (std::max)(socket, wakeEvent_.Socket());
			const int selected = select(static_cast<int>(maxSock) + 1, &readSet, nullptr, nullptr, &timeout);
			if (selected == SOCKET_ERROR) {
				break;
			}
			if (selected == 0) {
				continue;
			}

			if (FD_ISSET(wakeEvent_.Socket(), &readSet)) {
				wakeEvent_.Drain();
				if (stopRequested_.load()) {
					break;
				}
				continue;
			}

			if (FD_ISSET(socket, &readSet)) {
				if (!channel.RecvFrame(io, frame)) {
					log(__FUNCTION__, Logger::Level::Debug, L"Peer failed secure recv");
					break;
				}
				if (frame.size() < 4) {
					log(__FUNCTION__, Logger::Level::Warning, L"Peer: undersized frame; dropping connection.");
					break;
				}
				ReportTraffic(0, frame.size() + crypto_secretstream_xchacha20poly1305_ABYTES + sizeof(uint32_t));

				if (std::memcmp(frame.data(), "PONG", 4) == 0) {
					{
						std::lock_guard<std::mutex> lock(dataMutex_);
						lastPingReceivedAt_ = std::chrono::steady_clock::now();
					}
					log(__FUNCTION__, Logger::Level::DDebug, L"PONG");
				} else if (std::memcmp(frame.data(), "PING", 4) == 0) {
					if (!channel.SendFrame(io, "PONG")) {
						break;
					}
					ReportTraffic(4 + crypto_secretstream_xchacha20poly1305_ABYTES + sizeof(uint32_t), 0);
				} else if (std::memcmp(frame.data(), "SYNC", 4) == 0) {
					// Activity-stream sync request from the peer we're connected
					// to. Body is a 16-byte anchor GUID; respond with each item
					// after that anchor as a CLIP frame with FLAG_SYNC_REPLAY,
					// then an EOSY marker. Capped at clipboardSyncMaxItems.
					if (frame.size() != 4 + 16) {
						log(__FUNCTION__, Logger::Level::Warning,
							L"Rejecting SYNC frame: expected 20 bytes, got %zu.", frame.size());
						break;
					}
					std::array<uint8_t, 16> fromGuid{};
					std::memcpy(fromGuid.data(), frame.data() + 4, 16);

					const uint64_t maxItems = g_settings.clipboardSyncMaxItems();
					const auto replayItems = g_clipboardActivityStore.ItemsSince(fromGuid, maxItems);
					log(__FUNCTION__, Logger::Level::Info,
						L"Activity-stream sync requested; replaying %zu item(s).",
						replayItems.size());

					bool replayOk = true;
					for (const auto& replay : replayItems) {
						if (!replay) continue;
						NetworkDefs::ClipboardMessage netMsg = replay->meta;
						netMsg.flags |= NetworkDefs::CLPM_FLAG_SYNC_REPLAY;
						NetworkDefs::HostToNetworkClipboardMessage(netMsg);
						const auto& body = replay->EncodedBytes();
						if (!channel.SendFrame(io, "CLIP",
								reinterpret_cast<const unsigned char*>(&netMsg), sizeof(netMsg),
								body.data(), static_cast<uint32_t>(body.size()))) {
							replayOk = false;
							break;
						}
						ReportTraffic(4 + sizeof(netMsg) + body.size()
							+ crypto_secretstream_xchacha20poly1305_ABYTES + sizeof(uint32_t), 0);
					}
					if (!replayOk) {
						log(__FUNCTION__, Logger::Level::Warning, L"Activity-stream sync send failed mid-stream.");
						break;
					}
					if (!channel.SendFrame(io, "EOSY")) {
						log(__FUNCTION__, Logger::Level::Warning, L"Activity-stream sync EOSY send failed.");
						break;
					}
					ReportTraffic(4 + crypto_secretstream_xchacha20poly1305_ABYTES + sizeof(uint32_t), 0);
				} else if (std::memcmp(frame.data(), "EOSY", 4) == 0) {
					// Defensive: EOSY shouldn't normally arrive here (we're the
					// outgoing-direction peer; the requester is on the incoming
					// side). Tolerate it without breaking the connection.
					log(__FUNCTION__, Logger::Level::Debug, L"Unexpected EOSY received on outgoing connection; ignoring.");
				} else if (HandleRegisterFrame(channel, io, frame)) {
					// Register-protocol frame handled inline.
				} else {
					// Forward-compat: unknown tags from a peer with caps we don't recognize
					// are logged and ignored rather than treated as fatal.
					log(__FUNCTION__, Logger::Level::Warning, L"Peer: unknown frame tag '%c%c%c%c'; ignoring.",
						static_cast<wchar_t>(frame[0]), static_cast<wchar_t>(frame[1]),
						static_cast<wchar_t>(frame[2]), static_cast<wchar_t>(frame[3]));
				}
			}
		}

		// Inner loop exited: connection broke (or stopRequested). Drop the socket and either exit
		// (stopRequested) or fall through to the outer-loop reconnect attempt.
		CloseSocket();
		log(__FUNCTION__, Logger::Level::Info, L"Peer disconnected; will attempt to reconnect.");
		if (stopRequested_.load()) break;
		// We were connected at least once — start backoff from the bottom of the schedule.
		goBackoff();
	}

	CloseSocket();
	log(__FUNCTION__, Logger::Level::Info, L"Peer thread exiting.");
	running_.store(false);
	CullStoppedPeersAsync();
}

void Peer::ThreadProcRecv() {
	CryptoChannel channel;
	HostId remoteHostId;
	std::string remoteHostNameUtf8;
	SOCKET socket = CurrentSocket();
	const SocketIoContext io{ socket, wakeEvent_, stopRequested_ };
	if (socket != INVALID_SOCKET) {
		ConfigureTcpSocket(socket);
	}
	if (socket == INVALID_SOCKET || !SetSocketBlockingMode(socket, false) || !channel.ServerHandshake(io, remoteHostId, remoteHostNameUtf8)) {
		log(__FUNCTION__, Logger::Level::Error, L"Client secure handshake failed.");
	} else {
		{
			std::lock_guard<std::mutex> lock(dataMutex_);
			hostID_ = remoteHostId;
			hostName_ = Utf8ToWideString(remoteHostNameUtf8);
			osType_ = channel.RemoteOsType();
			remoteServesRegisters_.store((channel.RemoteCaps()[0] & CryptoChannel::CAP0_SERVES_REGISTERS) != 0);
		}
		if (verifiedCallback_) {
			verifiedCallback_(Utf8ToWideString(remoteHostNameUtf8), remoteHostId, channel.RemoteOsType(), connType_, createdAt_);
		}

		log(__FUNCTION__, Logger::Level::Info, L"Client connected");

		// Activity-stream sync trigger. If this is the first established incoming
		// peer, ask them for everything we don't have. Tail GUID anchors the
		// query; all-zero GUID (empty store) means "send me everything you have."
		const bool firstIncoming = g_peerManager.OnIncomingPeerEstablished();
		const bool incomingEstablished = true;
		if (firstIncoming) {
			const auto tail = g_clipboardActivityStore.TailEventGuid();
			if (channel.SendFrame(io, "SYNC", tail.data(), static_cast<uint32_t>(tail.size()))) {
				log(__FUNCTION__, Logger::Level::Info,
					L"Requested activity-stream sync from this peer (tail GUID supplied).");
				ReportTraffic(4 + tail.size() + crypto_secretstream_xchacha20poly1305_ABYTES + sizeof(uint32_t), 0);
			} else {
				log(__FUNCTION__, Logger::Level::Debug, L"Peer: failed to send initial SYNC request.");
			}
		}

		// Register anti-entropy with this incoming peer (per-connection, not gated on
		// firstIncoming): offer our digest so it pushes back anything we lack.
		SendRegisterSyncOnConnect(channel, io);

		// Idle deadline: if no traffic arrives for this long, assume the peer is gone (e.g.,
		// iOS backgrounded the app — the kernel keeps the TCP state alive but the process
		// can't respond, so the inner RecvFrame would otherwise block indefinitely).
		// Send-side picks up dead peers within one ping interval (~35s); recv-side mirrors
		// that with some headroom for jitter and slow networks.
		constexpr auto kRecvIdleTimeout = std::chrono::seconds(90);

		std::vector<unsigned char> frame;
		while (!stopRequested_.load()) {
			// Wrap the recv with a select() that wakes periodically so we can enforce the idle
			// deadline. RecvFrame's internal RecvAll uses an untimed select(), so
			// without this wrapper a stale-but-not-closed socket parks the thread forever.
			fd_set readSet;
			FD_ZERO(&readSet);
			FD_SET(io.socket, &readSet);
			FD_SET(wakeEvent_.Socket(), &readSet);

			timeval tv{};
			tv.tv_sec = 5;
			const SOCKET maxSock = (std::max)(io.socket, wakeEvent_.Socket());
			const int ready = select(static_cast<int>(maxSock) + 1, &readSet, nullptr, nullptr, &tv);
			if (ready == SOCKET_ERROR) {
				break;
			}
			if (stopRequested_.load()) {
				break;
			}
			if (ready > 0 && FD_ISSET(wakeEvent_.Socket(), &readSet)) {
				wakeEvent_.Drain();
				if (stopRequested_.load()) {
					break;
				}
			}
			if (ready == 0 || !FD_ISSET(io.socket, &readSet)) {
				const auto silence = std::chrono::steady_clock::now() - lastPingReceivedAt();
				if (silence >= kRecvIdleTimeout) {
					log(__FUNCTION__, Logger::Level::Info,
						L"Incoming peer idle for %lld s; closing connection.",
						static_cast<long long>(std::chrono::duration_cast<std::chrono::seconds>(silence).count()));
					break;
				}
				continue;
			}

			if (io.socket == INVALID_SOCKET || !channel.RecvFrame(io, frame)) {
				break;
			}
			if (frame.size() < 4) {
				log(__FUNCTION__, Logger::Level::Warning, L"Rejecting undersized frame (%zu bytes).", frame.size());
				break;
			}
			ReportTraffic(0, frame.size() + crypto_secretstream_xchacha20poly1305_ABYTES + sizeof(uint32_t));

			if (std::memcmp(frame.data(), "PING", 4) == 0) {
				log(__FUNCTION__, Logger::Level::DDebug, L"PING");
				{
					std::lock_guard<std::mutex> lock(dataMutex_);
					lastPingReceivedAt_ = std::chrono::steady_clock::now();
				}
				if (!channel.SendFrame(io, "PONG")) {
					break;
				}
				log(__FUNCTION__, Logger::Level::DDebug, L"PONG!");
				ReportTraffic(4 + crypto_secretstream_xchacha20poly1305_ABYTES + sizeof(uint32_t), 0);
				continue;
			}

			if (std::memcmp(frame.data(), "CLIP", 4) == 0) {
				{
					std::lock_guard<std::mutex> lock(dataMutex_);
					lastPingReceivedAt_ = std::chrono::steady_clock::now();
				}

				ClipboardPayload payload;
				if (!ClipboardWire::TryDecodeClipboardFrame(frame, payload)) {
					break;
				}

				log(__FUNCTION__, Logger::Level::Debug,
					L"Clipboard message received: format %ls (%u), compressed=%u, payload size=%llu bytes, uncompressed size=%llu bytes",
					ClippClipboardFormatNameW(payload.meta.formatId),
					payload.meta.formatId,
					payload.meta.isCompressed,
					static_cast<unsigned long long>(payload.meta.payloadDataSize),
					static_cast<unsigned long long>(payload.meta.uncompressedDataSize));

				if (clipboardReceivedCallback_) {
					clipboardReceivedCallback_(std::make_shared<const ClipboardPayload>(std::move(payload)));
				}
				continue;
			}

			if (std::memcmp(frame.data(), "EOSY", 4) == 0) {
				// End-of-sync marker from the peer that fulfilled our SYNC
				// request. Nothing structural to do — each replayed CLIP has
				// already been folded into the activity store via dedup-aware
				// insert. Logged for diagnostics.
				log(__FUNCTION__, Logger::Level::Info, L"Activity-stream sync from this peer complete.");
				continue;
			}

			if (std::memcmp(frame.data(), "RCNT", 4) == 0) {
				// A one-shot client (e.g. `clipp paste`) requesting our most recent
				// clipboard item. Reply with a single CLIP frame, or NONE when we
				// have nothing servable. Text-only: the CLI pipes bytes to stdout and
				// has no way to emit an image, so a non-text newest item reads as
				// "nothing". Unlike SYNC this is one item and no EOSY.
				const std::array<uint8_t, 16> zero{};
				const auto recent = g_clipboardActivityStore.ItemsSince(zero, 1);
				const bool servable = !recent.empty() && recent.back()
					&& !recent.back()->Empty()
					&& recent.back()->meta.formatId == CLIPP_FORMAT_UTF8;
				if (servable) {
					if (!SendClipboardData(channel, io, *recent.back())) {
						log(__FUNCTION__, Logger::Level::Debug, L"Failed to send RCNT response.");
						break;
					}
				} else if (!channel.SendFrame(io, "NONE")) {
					break;
				}
				continue;
			}

			if (std::memcmp(frame.data(), "NMAP", 4) == 0) {
#if CLIPP_REGISTERS_DAEMON
				// `clipp map`: this daemon's connection table (the same per-host
				// aggregates the GUI peer list renders) as extensible key=value
				// text. One record per line; `name=` is always the LAST key and
				// takes the rest of the line (device names may contain spaces);
				// readers ignore unknown keys, so fields can be added without a
				// protocol rev. Names are peer-controlled: scrub control chars at
				// the emitter so a hostile name can't inject fake report lines.
				const auto scrub = [](std::string s) {
					for (char& c : s) {
						const unsigned char u = static_cast<unsigned char>(c);
						if (u < 0x20 || u == 0x7f) c = '.';
					}
					return s;
				};
				const auto now = std::chrono::steady_clock::now();
				HostId selfId{};
				g_settings.getHostID(selfId);
				std::string report = "netmap v=1\n";
				report += "self ver=" CLIPP_VERSION_STRING " id=" +
					WideToUtf8String(selfId.ToHexWString()) + " name=" +
					scrub(clipp::GetLocalPeerDisplayName("unknown", CryptoChannel::HOSTNAME_MAX_BYTES)) + "\n";
				for (const PeerDisplayItem& item : g_peerDisplay.Query()) {
					size_t inCount = item.incomingConnectionCount;
					const size_t outCount = item.outgoingConnectionCount;
					// The connection carrying this request is measurement
					// apparatus, not mesh state: subtract it. (It is an established
					// incoming connection from the requester's hostId — its
					// NotifyPeer ran at handshake, before this handler.)
					if (item.hostID == hostID() && inCount > 0) --inCount;
					if (inCount == 0 && outCount == 0) continue;
					const char* state = "connected";
					switch (item.outgoingConnState) {
					case PeerConnState::Connecting: state = "connecting"; break;
					case PeerConnState::Connected:  state = "connected";  break;
					case PeerConnState::Backoff:    state = "backoff";    break;
					case PeerConnState::Failed:     state = "failed";     break;
					}
					const char* os = "unknown";
					switch (item.osType) {
					case OsType::Windows:    os = "windows"; break;
					case OsType::MacOS:      os = "macos";   break;
					case OsType::IOS_iPhone: os = "iphone";  break;
					case OsType::IOS_iPad:   os = "ipad";    break;
					case OsType::Linux:      os = "linux";   break;
					default: break;
					}
					uint64_t ageSeconds = 0;
					if (item.connectedSince != std::chrono::steady_clock::time_point{} &&
						now > item.connectedSince) {
						ageSeconds = static_cast<uint64_t>(
							std::chrono::duration_cast<std::chrono::seconds>(now - item.connectedSince).count());
					}
					char fields[192];
					std::snprintf(fields, sizeof(fields),
						"conn in=%zu out=%zu state=%s age=%llu tx=%llu rx=%llu os=%s id=",
						inCount, outCount, state,
						static_cast<unsigned long long>(ageSeconds),
						static_cast<unsigned long long>(item.bytesSent),
						static_cast<unsigned long long>(item.bytesReceived), os);
					report += fields;
					report += WideToUtf8String(item.hostID.ToHexWString());
					report += " name=" + scrub(WideToUtf8String(item.hostName)) + "\n";
				}
				if (!channel.SendFrame(io, "NMAP",
						reinterpret_cast<const unsigned char*>(report.data()),
						static_cast<uint32_t>(report.size()))) {
					break;
				}
#else
				// Non-daemon builds never advertise the cap; answer a confused
				// client definitively rather than leaving it to time out.
				if (!channel.SendFrame(io, "NONE")) {
					break;
				}
#endif
				continue;
			}

			if (HandleRegisterFrame(channel, io, frame)) {
				continue;
			}

			// Forward-compat: unknown tag from a peer that may speak future caps.
			// Log and keep the connection alive — the frame body has already been
			// consumed by RecvFrame so the stream stays in sync.
			log(__FUNCTION__, Logger::Level::Warning,
				L"Peer: unknown frame tag '%c%c%c%c' (%zu byte body); ignoring.",
				static_cast<wchar_t>(frame[0]), static_cast<wchar_t>(frame[1]),
				static_cast<wchar_t>(frame[2]), static_cast<wchar_t>(frame[3]),
				frame.size() - 4);
		}
		if (incomingEstablished) {
			g_peerManager.OnIncomingPeerLeft();
		}
		log(__FUNCTION__, Logger::Level::Info, L"Client disconnected");
	}

	CloseSocket();
	running_.store(false);
	CullStoppedPeersAsync();
	log(__FUNCTION__, Logger::Level::Info, L"Thread exiting");
}
