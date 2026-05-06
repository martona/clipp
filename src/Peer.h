#pragma once

#include <array>
#include <cstdarg>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector> 
#include <memory> 
#include <variant>
#include "platform.h"
#include "BlockingQueue.h"
#include "ClipboardData.h"
#include "Logger.h"
class CryptoChannel;

class Peer {
public:
	Peer(const wchar_t* hostName, const unsigned char* hostID, const wchar_t* ip, unsigned short port);
	~Peer();

	void Start();
	void Stop();
	bool isRunning() const;

	std::wstring hostName() const;
	std::array<unsigned char, 32> hostID() const;
	std::wstring ip() const;
	unsigned short port() const;
	std::chrono::steady_clock::time_point lastPingReceivedAt() const;
	std::chrono::steady_clock::time_point createdAt() const;
	void PushMessage(std::shared_ptr<const ClipboardPayload> payload) { messageQueue_.Push(std::move(payload)); }

private:
	void ThreadProc();
	bool ConnectSocket();
	void CloseSocket();
	bool SendClipboardData(CryptoChannel& channel, const ClipboardPayload& payload);
	void InterruptibleSleep(std::chrono::milliseconds duration);
	void log(const char* function, Logger::Level level, const wchar_t* message, ...) const;
	void logV(const char* function, Logger::Level level, const wchar_t* message, va_list args) const;

	mutable std::mutex dataMutex_;
	std::wstring hostName_;
	std::wstring ip_;
	unsigned short port_{};
	std::array<unsigned char, 32> hostID_{};
	std::chrono::steady_clock::time_point createdAt_;
	std::chrono::steady_clock::time_point lastPingReceivedAt_;

	std::thread thread_;
	std::atomic<bool> stopRequested_{ false };
	std::atomic<bool> running_{ false };
	std::mutex stopMutex_;
	std::condition_variable stopCV_;
	
	BlockingQueue<std::shared_ptr<const ClipboardPayload>> messageQueue_;

	SOCKET socket_{ INVALID_SOCKET };
};
