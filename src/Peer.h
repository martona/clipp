#pragma once

#include <array>
#include <cstdarg>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector> 
#include <memory> 
#include <variant>
#include <functional>
#include "platform.h"
#include "utils_socket.h"
#include "BlockingQueue.h"
#include "ClipboardPayload.h"
#include "Logger.h"
#include "HostId.h"
#include "OsType.h"

class CryptoChannel;

// A pre-encoded frame (4-byte tag + body) queued for a peer's send thread,
// alongside clipboard payloads. Carries register-protocol broadcasts (REGW).
struct OutboundRawFrame {
	std::array<char, 4> tag{};
	std::vector<unsigned char> body;
};
using OutboundMessage = std::variant<std::shared_ptr<const ClipboardPayload>, OutboundRawFrame>;

class Peer {
public:
	typedef enum class ConnType {
		Outgoing,
		Incoming,
	} ConnType;
	ConnType connType_;

	using ClipboardReceivedCallback = std::function<void(std::shared_ptr<const ClipboardPayload>)>;
	using VerifiedCallback = std::function<void(const std::wstring&, const HostId&, OsType, ConnType, std::chrono::steady_clock::time_point)>;
	using TrafficCallback = std::function<void(const HostId&, uint64_t, uint64_t)>;

	// peers we're connecting to
	Peer(const wchar_t* hostName, const HostId* hostID, const wchar_t* ip, unsigned short port, VerifiedCallback verifiedCallback = nullptr, TrafficCallback trafficCallback = nullptr);
	// peers that connected to us
	Peer(SOCKET socket, ClipboardReceivedCallback clipboardReceivedCallback, VerifiedCallback verifiedCallback = nullptr, TrafficCallback trafficCallback = nullptr);
	~Peer();

	void Start();
	void Stop();
	bool isRunning() const;

	std::wstring hostName() const;
	HostId hostID() const;
	// The remote's self-reported OS, learned at handshake. OsType::Unknown until
	// then (and for outgoing peers, which carry it but never surface it).
	OsType osType() const;
	std::wstring ip() const;
	unsigned short port() const;
	std::chrono::steady_clock::time_point lastPingReceivedAt() const;
	std::chrono::steady_clock::time_point createdAt() const;
	void PushMessage(std::shared_ptr<const ClipboardPayload> payload);
	// Queue a pre-encoded frame (e.g. a register REGW broadcast) for this peer's
	// send thread. Like PushMessage, only effective for outgoing peers (their send
	// loop drains the queue); harmless on incoming peers.
	void PushRawFrame(std::array<char, 4> tag, std::vector<unsigned char> body);
	// True once the handshake has shown the remote advertises CAP0_SERVES_REGISTERS.
	bool RemoteServesRegisters() const { return remoteServesRegisters_.load(); }

private:
	void ThreadProcRecv();
	void ThreadProcSend();
	bool ConnectSocket();
	void CloseSocket();
	bool SendClipboardData(CryptoChannel& channel, const SocketIoContext& io, const ClipboardPayload& payload);
	bool DrainOutboundMessages(CryptoChannel& channel, const SocketIoContext& io);
	// Register-protocol anti-entropy, handled inline in the recv loops (no send
	// queue involved). HandleRegisterFrame returns true if the frame was a register
	// tag it consumed. Both are no-ops on builds that don't run the register daemon.
	bool HandleRegisterFrame(CryptoChannel& channel, const SocketIoContext& io, const std::vector<unsigned char>& frame);
	void SendRegisterSyncOnConnect(CryptoChannel& channel, const SocketIoContext& io);
	void ReportTraffic(uint64_t bytesSent, uint64_t bytesReceived);
	SOCKET CurrentSocket() const;
	void SetSocket(SOCKET socket);
	SOCKET DetachSocket();
	void InterruptibleSleep(std::chrono::milliseconds duration);
	void log(const char* function, Logger::Level level, const wchar_t* message, ...) const;
	void logV(const char* function, Logger::Level level, const wchar_t* message, va_list args) const;
	const wchar_t* ConnTypeString() const;

	mutable std::mutex dataMutex_;
	uint64_t logId_{};
	std::wstring hostName_;
	std::wstring ip_;
	unsigned short port_{};
	HostId hostID_;
	OsType osType_{ OsType::Unknown };
	std::chrono::steady_clock::time_point createdAt_;
	std::chrono::steady_clock::time_point lastPingReceivedAt_;

	std::thread thread_;
	std::atomic<bool> stopRequested_{ false };
	std::atomic<bool> running_{ false };
	std::atomic<bool> remoteServesRegisters_{ false };
	std::mutex stopMutex_;
	std::condition_variable stopCV_;
	
	BlockingQueue<OutboundMessage> messageQueue_;
	ClipboardReceivedCallback clipboardReceivedCallback_{};
	VerifiedCallback verifiedCallback_{};
	TrafficCallback trafficCallback_{};

	SOCKET socket_{ INVALID_SOCKET };
	mutable std::mutex socketMutex_;
	SocketWakeEvent wakeEvent_;
};
