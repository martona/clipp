#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <string>

#include "platform.h"

#ifndef _WIN32
#include <fcntl.h>
#endif

class SocketWakeEvent {
public:
	SocketWakeEvent() = default;
	SocketWakeEvent(const SocketWakeEvent&) = delete;
	SocketWakeEvent& operator=(const SocketWakeEvent&) = delete;

	~SocketWakeEvent() {
		Close();
	}

	bool Initialize() {
		Close();

		socket_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
		if (socket_ == INVALID_SOCKET)
			return false;

		sockaddr_in bindAddr{};
		bindAddr.sin_family = AF_INET;
		bindAddr.sin_port = 0;
		if (inet_pton(AF_INET, "127.0.0.1", &bindAddr.sin_addr) != 1) {
			Close();
			return false;
		}

		if (bind(socket_, reinterpret_cast<const sockaddr*>(&bindAddr), sizeof(bindAddr)) == SOCKET_ERROR) {
			Close();
			return false;
		}

		socklen_t addrLen = sizeof(address_);
		if (getsockname(socket_, reinterpret_cast<sockaddr*>(&address_), &addrLen) == SOCKET_ERROR) {
			Close();
			return false;
		}

		return true;
	}

	void Close() {
		if (socket_ != INVALID_SOCKET) {
			closesocket(socket_);
			socket_ = INVALID_SOCKET;
		}
		address_ = {};
	}

	bool IsValid() const {
		return socket_ != INVALID_SOCKET;
	}

	SOCKET Socket() const {
		return socket_;
	}

	void Signal() const {
		if (socket_ == INVALID_SOCKET)
			return;

		const char wakeByte = 0;
		sendto(socket_, &wakeByte, sizeof(wakeByte), 0,
			reinterpret_cast<const sockaddr*>(&address_), sizeof(address_));
	}

	void Drain() const {
		if (socket_ == INVALID_SOCKET)
			return;

		std::array<char, 64> buffer{};
		for (;;) {
			fd_set readSet;
			FD_ZERO(&readSet);
			FD_SET(socket_, &readSet);

			timeval timeout{};
			const int ready = select(static_cast<int>(socket_) + 1, &readSet, nullptr, nullptr, &timeout);
			if (ready <= 0 || !FD_ISSET(socket_, &readSet))
				break;

			sockaddr_in fromAddr{};
			socklen_t fromLen = sizeof(fromAddr);
			const auto received = recvfrom(socket_, buffer.data(), static_cast<int>(buffer.size()), 0,
				reinterpret_cast<sockaddr*>(&fromAddr), &fromLen);
			if (received <= 0)
				break;
		}
	}

private:
	SOCKET socket_{ INVALID_SOCKET };
	sockaddr_in address_{};
};

struct SocketIoContext {
	SocketIoContext(SOCKET socketIn, const SocketWakeEvent& wakeEventIn, const std::atomic<bool>& stopRequestedIn)
		: socket(socketIn), wakeEvent(wakeEventIn), stopRequested(stopRequestedIn) {}

	SOCKET socket;
	const SocketWakeEvent& wakeEvent;
	const std::atomic<bool>& stopRequested;
};

enum class SocketWaitResult {
	Ready,
	Woken,
	Failed,
};

static inline int LastSocketError() {
#ifdef _WIN32
	return WSAGetLastError();
#else
	return errno;
#endif
}

static inline bool SocketWouldBlock(int error) {
#ifdef _WIN32
	return error == WSAEWOULDBLOCK;
#else
	return error == EWOULDBLOCK || error == EAGAIN;
#endif
}

static inline bool SocketInterrupted(int error) {
#ifdef _WIN32
	return error == WSAEINTR;
#else
	return error == EINTR;
#endif
}

static inline bool SetSocketBlockingMode(SOCKET socket, bool blocking) {
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

static inline bool IsConnectPendingError(int error) {
#ifdef _WIN32
	return error == WSAEWOULDBLOCK || error == WSAEINPROGRESS || error == WSAEALREADY;
#else
	return error == EINPROGRESS || error == EWOULDBLOCK || error == EALREADY;
#endif
}

static inline bool GetPendingConnectError(SOCKET socket, int& connectError) {
	socklen_t optionLength = sizeof(connectError);
	connectError = 0;
	return getsockopt(socket, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&connectError), &optionLength) == 0;
}

static inline SOCKET ConnectTcpSocket(const std::string& ip, unsigned short port, std::chrono::milliseconds timeout) {
	SOCKET socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (socket == INVALID_SOCKET) {
		return INVALID_SOCKET;
	}

	if (!SetSocketBlockingMode(socket, false)) {
		closesocket(socket);
		return INVALID_SOCKET;
	}

	sockaddr_in address{};
	address.sin_family = AF_INET;
	address.sin_port = htons(port);
	if (inet_pton(AF_INET, ip.c_str(), &address.sin_addr) != 1) {
		closesocket(socket);
		return INVALID_SOCKET;
	}

	if (connect(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0) {
		return socket;
	}

	if (!IsConnectPendingError(LastSocketError())) {
		closesocket(socket);
		return INVALID_SOCKET;
	}

	fd_set writeSet;
	fd_set errorSet;
	FD_ZERO(&writeSet);
	FD_ZERO(&errorSet);
	FD_SET(socket, &writeSet);
	FD_SET(socket, &errorSet);

	timeval wait{};
	wait.tv_sec = static_cast<long>(timeout.count() / 1000);
	wait.tv_usec = static_cast<decltype(wait.tv_usec)>((timeout.count() % 1000) * 1000);

	const int ready = select(static_cast<int>(socket) + 1, nullptr, &writeSet, &errorSet, &wait);
	if (ready <= 0) {
		closesocket(socket);
		return INVALID_SOCKET;
	}

	int connectError = 0;
	if (!GetPendingConnectError(socket, connectError) || connectError != 0) {
		closesocket(socket);
		return INVALID_SOCKET;
	}

	return socket;
}

static inline SocketWaitResult WaitForSocket(SOCKET sock, const SocketWakeEvent* wakeEvent, bool readable, bool writable) {
	fd_set readSet;
	fd_set writeSet;
	FD_ZERO(&readSet);
	FD_ZERO(&writeSet);

	if (readable)
		FD_SET(sock, &readSet);
	if (writable)
		FD_SET(sock, &writeSet);

	SOCKET maxSock = sock;
	if (wakeEvent != nullptr && wakeEvent->IsValid()) {
		FD_SET(wakeEvent->Socket(), &readSet);
		maxSock = (std::max)(maxSock, wakeEvent->Socket());
	}

	const int ready = select(static_cast<int>(maxSock) + 1,
		readable || (wakeEvent != nullptr && wakeEvent->IsValid()) ? &readSet : nullptr,
		writable ? &writeSet : nullptr,
		nullptr,
		nullptr);
	if (ready == SOCKET_ERROR)
		return SocketWaitResult::Failed;

	if (wakeEvent != nullptr && wakeEvent->IsValid() && FD_ISSET(wakeEvent->Socket(), &readSet)) {
		wakeEvent->Drain();
		return SocketWaitResult::Woken;
	}

	if ((readable && FD_ISSET(sock, &readSet)) || (writable && FD_ISSET(sock, &writeSet)))
		return SocketWaitResult::Ready;

	return SocketWaitResult::Failed;
}

static inline unsigned short SocketPeerPort(SOCKET socket) {
	sockaddr_storage address{};
	socklen_t addressLength = sizeof(address);
	if (getpeername(socket, reinterpret_cast<sockaddr*>(&address), &addressLength) != 0) {
		return 0;
	}

	if (address.ss_family == AF_INET) {
		const auto* address4 = reinterpret_cast<const sockaddr_in*>(&address);
		return ntohs(address4->sin_port);
	}

	if (address.ss_family == AF_INET6) {
		const auto* address6 = reinterpret_cast<const sockaddr_in6*>(&address);
		return ntohs(address6->sin6_port);
	}

	return 0;
}

static inline std::string SocketPeerIp(SOCKET socket) {
	sockaddr_storage address{};
	socklen_t addressLength = sizeof(address);
	if (getpeername(socket, reinterpret_cast<sockaddr*>(&address), &addressLength) != 0) {
		return "";
	}

	char ip[INET6_ADDRSTRLEN]{};
	if (address.ss_family == AF_INET) {
		const auto* address4 = reinterpret_cast<const sockaddr_in*>(&address);
		if (inet_ntop(AF_INET, &address4->sin_addr, ip, sizeof(ip)) == nullptr) {
			return "";
		}
	}
	else if (address.ss_family == AF_INET6) {
		const auto* address6 = reinterpret_cast<const sockaddr_in6*>(&address);
		if (inet_ntop(AF_INET6, &address6->sin6_addr, ip, sizeof(ip)) == nullptr) {
			return "";
		}
	}
	else {
		return "";
	}

	return ip;
}

static inline bool RecvAll(const SocketIoContext& io, char* buffer, int length) {
	int total = 0;
	while (total < length) {
		if (io.stopRequested.load())
			return false;
		const SocketWaitResult waitResult = WaitForSocket(io.socket, &io.wakeEvent, true, false);
		if (waitResult == SocketWaitResult::Failed)
			return false;
		if (waitResult == SocketWaitResult::Woken)
			continue;

		const int remaining = length - total;
		const auto received = recv(io.socket, buffer + total, remaining, 0);
		if (received > 0) {
			total += static_cast<int>(received);
			continue;
		}
		if (received == 0) {
			return false;
		}
		const int error = LastSocketError();
		if (SocketWouldBlock(error) || SocketInterrupted(error)) {
			continue;
		}
		return false;
	}
	return true;
}

static inline bool SendAll(const SocketIoContext& io, const char* buffer, int length) {
	int total = 0;
	while (total < length) {
		if (io.stopRequested.load())
			return false;
		const SocketWaitResult waitResult = WaitForSocket(io.socket, &io.wakeEvent, false, true);
		if (waitResult == SocketWaitResult::Failed)
			return false;
		if (waitResult == SocketWaitResult::Woken)
			continue;

		const int chunkLength = (std::min)(length - total, 64 * 1024);
		const auto sent = send(io.socket, buffer + total, chunkLength, 0);
		if (sent > 0) {
			total += static_cast<int>(sent);
			continue;
		}
		if (sent == 0) {
			return false;
		}
		const int error = LastSocketError();
		if (SocketWouldBlock(error) || SocketInterrupted(error)) {
			continue;
		}
		return false;
	}
	return true;
}
