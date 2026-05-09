#pragma once

#include <algorithm>
#include <array>
#include <cerrno>
#include <string>

#include "platform.h"

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
			const int received = recvfrom(socket_, buffer.data(), static_cast<int>(buffer.size()), 0,
				reinterpret_cast<sockaddr*>(&fromAddr), &fromLen);
			if (received <= 0)
				break;
		}
	}

private:
	SOCKET socket_{ INVALID_SOCKET };
	sockaddr_in address_{};
};

enum class SocketWaitResult {
	Ready,
	Woken,
	Failed,
};

static int LastSocketError() {
#ifdef _WIN32
	return WSAGetLastError();
#else
	return errno;
#endif
}

static bool SocketWouldBlock(int error) {
#ifdef _WIN32
	return error == WSAEWOULDBLOCK;
#else
	return error == EWOULDBLOCK || error == EAGAIN;
#endif
}

static bool SocketInterrupted(int error) {
#ifdef _WIN32
	return error == WSAEINTR;
#else
	return error == EINTR;
#endif
}

static SocketWaitResult WaitForSocket(SOCKET sock, const SocketWakeEvent* wakeEvent, bool readable, bool writable) {
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

static unsigned short SocketPeerPort(SOCKET socket) {
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

static std::string SocketPeerIp(SOCKET socket) {
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

static bool RecvAll(SOCKET sock, char* buffer, int length, const SocketWakeEvent* wakeEvent = nullptr) {
	long total = 0;
	while (total < length) {
		if (wakeEvent != nullptr) {
			const SocketWaitResult waitResult = WaitForSocket(sock, wakeEvent, true, false);
			if (waitResult != SocketWaitResult::Ready)
				return false;
		}
		const int received = recv(sock, buffer + total, (int)(length - total), 0);
		if (received > 0) {
			total += received;
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

static bool SendAll(SOCKET sock, const char* buffer, int length, const SocketWakeEvent* wakeEvent = nullptr) {
	long total = 0;
	while (total < length) {
		if (wakeEvent != nullptr) {
			const SocketWaitResult waitResult = WaitForSocket(sock, wakeEvent, false, true);
			if (waitResult != SocketWaitResult::Ready)
				return false;
		}
		const int chunkLength = (std::min)(length - static_cast<int>(total), 64 * 1024);
		const int sent = send(sock, buffer + total, chunkLength, 0);
		if (sent > 0) {
			total += sent;
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
