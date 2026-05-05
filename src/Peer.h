#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector> 
#include <memory> 
#include <variant>
#include "platform.h"
#include "BlockingQueue.h"
#include "ClipboardData.h"
class CryptoChannel;

class Peer {
public:
	Peer(const wchar_t* hostName, const unsigned char* hostID, const char* ip, unsigned short port);
	~Peer();

	void Start();
	void Stop();

	std::wstring hostName() const;
	std::array<unsigned char, 32> hostID() const;
	std::wstring ipw() const;
	std::string ip() const;
	unsigned short port() const;
	std::chrono::steady_clock::time_point lastPingReceivedAt() const;
	std::chrono::steady_clock::time_point createdAt() const;
	void PushMessage(std::shared_ptr<const ClipboardPayload> payload) { messageQueue_.Push(std::move(payload)); }

private:
	void ThreadProc();
	bool ConnectSocket();
	void CloseSocket();
	bool SendHello();
	bool SendClipboardData(CryptoChannel& channel, const ClipboardPayload& payload);
	void InterruptibleSleep(std::chrono::milliseconds duration);

	mutable std::mutex dataMutex_;
	std::wstring hostName_;
	std::array<unsigned char, 32> hostID_{};
	std::string ip_;
	unsigned short port_{};
	std::chrono::steady_clock::time_point createdAt_;
	std::chrono::steady_clock::time_point lastPingReceivedAt_;

	std::thread thread_;
	std::atomic<bool> stopRequested_{ false };
	std::mutex stopMutex_;
	std::condition_variable stopCV_;
	
	BlockingQueue<std::shared_ptr<const ClipboardPayload>> messageQueue_;

	SOCKET socket_{ INVALID_SOCKET };
};
