#include <doctest/doctest.h>

#include "utils_socket.h"

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;

class SocketRuntime {
public:
	SocketRuntime() {
#ifdef _WIN32
		WSADATA data{};
		initialized_ = WSAStartup(MAKEWORD(2, 2), &data) == 0;
#else
		initialized_ = true;
#endif
	}

	SocketRuntime(const SocketRuntime&) = delete;
	SocketRuntime& operator=(const SocketRuntime&) = delete;

	~SocketRuntime() {
#ifdef _WIN32
		if (initialized_) {
			WSACleanup();
		}
#endif
	}

	bool IsInitialized() const {
		return initialized_;
	}

private:
	bool initialized_{ false };
};

class ConnectedTcpPair {
public:
	ConnectedTcpPair() = default;
	ConnectedTcpPair(const ConnectedTcpPair&) = delete;
	ConnectedTcpPair& operator=(const ConnectedTcpPair&) = delete;

	~ConnectedTcpPair() {
		Close();
	}

	bool Open() {
		Close();

		SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (listener == INVALID_SOCKET) {
			return false;
		}

		sockaddr_in address{};
		address.sin_family = AF_INET;
		address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		address.sin_port = 0;
		if (bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR
			|| listen(listener, 1) == SOCKET_ERROR) {
			closesocket(listener);
			return false;
		}

		socklen_t addressLength = sizeof(address);
		if (getsockname(listener, reinterpret_cast<sockaddr*>(&address), &addressLength) == SOCKET_ERROR) {
			closesocket(listener);
			return false;
		}

		client_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (client_ == INVALID_SOCKET
			|| connect(client_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR) {
			closesocket(listener);
			Close();
			return false;
		}

		server_ = accept(listener, nullptr, nullptr);
		closesocket(listener);
		if (server_ == INVALID_SOCKET
			|| !SetSocketBlockingMode(client_, false)
			|| !SetSocketBlockingMode(server_, false)) {
			Close();
			return false;
		}

		return true;
	}

	void Close() {
		if (client_ != INVALID_SOCKET) {
			closesocket(client_);
			client_ = INVALID_SOCKET;
		}
		if (server_ != INVALID_SOCKET) {
			closesocket(server_);
			server_ = INVALID_SOCKET;
		}
	}

	SOCKET Client() const {
		return client_;
	}

	SOCKET Server() const {
		return server_;
	}

private:
	SOCKET client_{ INVALID_SOCKET };
	SOCKET server_{ INVALID_SOCKET };
};

bool SendByte(SOCKET socket, char value) {
	for (int attempt = 0; attempt < 100; ++attempt) {
		const auto sent = send(socket, &value, 1, SocketSendFlags());
		if (sent == 1) {
			return true;
		}
		if (sent == SOCKET_ERROR) {
			const int error = LastSocketError();
			if (SocketWouldBlock(error) || SocketInterrupted(error)) {
				std::this_thread::sleep_for(1ms);
				continue;
			}
		}
		return false;
	}
	return false;
}

bool SetSocketBufferSize(SOCKET socket, int option, int size) {
	return setsockopt(
		socket,
		SOL_SOCKET,
		option,
		reinterpret_cast<const char*>(&size),
		sizeof(size)) == 0;
}

}  // namespace

TEST_CASE("RecvAll drops a connection after an idle receive interval") {
	SocketRuntime runtime;
	REQUIRE(runtime.IsInitialized());

	ConnectedTcpPair sockets;
	REQUIRE(sockets.Open());

	SocketWakeEvent wakeEvent;
	REQUIRE(wakeEvent.Initialize());

	std::atomic<bool> stopRequested{ false };
	SocketIoContext io(sockets.Server(), wakeEvent, stopRequested, 200ms);
	char received = '\0';

	const auto startedAt = std::chrono::steady_clock::now();
	const bool result = RecvAll(io, &received, 1);
	const auto elapsed = std::chrono::steady_clock::now() - startedAt;

	CHECK_FALSE(result);
	CHECK(elapsed >= 100ms);
	CHECK(elapsed < 2s);
}

TEST_CASE("RecvAll restarts its idle interval after byte-level progress") {
	SocketRuntime runtime;
	REQUIRE(runtime.IsInitialized());

	ConnectedTcpPair sockets;
	REQUIRE(sockets.Open());

	SocketWakeEvent wakeEvent;
	REQUIRE(wakeEvent.Initialize());

	std::atomic<bool> writerSucceeded{ true };
	std::thread writer([&] {
		std::this_thread::sleep_for(150ms);
		if (!SendByte(sockets.Client(), 'a')) {
			writerSucceeded.store(false);
		}
		std::this_thread::sleep_for(150ms);
		if (!SendByte(sockets.Client(), 'b')) {
			writerSucceeded.store(false);
		}
	});

	std::atomic<bool> stopRequested{ false };
	SocketIoContext io(sockets.Server(), wakeEvent, stopRequested, 250ms);
	char received[2]{};

	const auto startedAt = std::chrono::steady_clock::now();
	const bool result = RecvAll(io, received, sizeof(received));
	const auto elapsed = std::chrono::steady_clock::now() - startedAt;
	writer.join();

	CHECK(writerSucceeded.load());
	CHECK(result);
	CHECK(received[0] == 'a');
	CHECK(received[1] == 'b');
	CHECK(elapsed >= 250ms);
	CHECK(elapsed < 2s);
}

TEST_CASE("RecvAll does not restart an absolute deadline after progress") {
	SocketRuntime runtime;
	REQUIRE(runtime.IsInitialized());

	ConnectedTcpPair sockets;
	REQUIRE(sockets.Open());

	SocketWakeEvent wakeEvent;
	REQUIRE(wakeEvent.Initialize());

	std::atomic<bool> writerSucceeded{ true };
	std::thread writer([&] {
		std::this_thread::sleep_for(100ms);
		if (!SendByte(sockets.Client(), 'a')) {
			writerSucceeded.store(false);
		}
		std::this_thread::sleep_for(150ms);
		if (!SendByte(sockets.Client(), 'b')) {
			writerSucceeded.store(false);
		}
	});

	std::atomic<bool> stopRequested{ false };
	const auto absoluteDeadline = std::chrono::steady_clock::now() + 200ms;
	SocketIoContext io(sockets.Server(), wakeEvent, stopRequested, 500ms, absoluteDeadline);
	char received[2]{};

	const auto startedAt = std::chrono::steady_clock::now();
	const bool result = RecvAll(io, received, sizeof(received));
	const auto finishedAt = std::chrono::steady_clock::now();
	writer.join();

	CHECK(writerSucceeded.load());
	CHECK_FALSE(result);
	CHECK(received[0] == 'a');
	CHECK(finishedAt - startedAt >= 100ms);
	CHECK(finishedAt - startedAt < 1s);
}

TEST_CASE("SendAll drops a connection after an idle send interval") {
	SocketRuntime runtime;
	REQUIRE(runtime.IsInitialized());

	ConnectedTcpPair sockets;
	REQUIRE(sockets.Open());
	REQUIRE(SetSocketBufferSize(sockets.Client(), SO_SNDBUF, 4096));
	REQUIRE(SetSocketBufferSize(sockets.Server(), SO_RCVBUF, 4096));

	SocketWakeEvent wakeEvent;
	REQUIRE(wakeEvent.Initialize());

	std::atomic<bool> stopRequested{ false };
	SocketIoContext io(
		sockets.Client(),
		wakeEvent,
		stopRequested,
		200ms,
		std::chrono::steady_clock::now() + 3s);
	std::vector<char> payload(16 * 1024 * 1024, 'x');

	const auto startedAt = std::chrono::steady_clock::now();
	const bool result = SendAll(io, payload.data(), static_cast<int>(payload.size()));
	const auto elapsed = std::chrono::steady_clock::now() - startedAt;

	CHECK_FALSE(result);
	CHECK(elapsed >= 100ms);
	CHECK(elapsed < 2s);
}
