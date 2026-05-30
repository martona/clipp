#include "OneShotPeer.h"

#include "ClipboardWire.h"
#include "Logger.h"

OneShotPeer::~OneShotPeer() {
    Teardown();
}

bool OneShotPeer::Connect(const std::string& ip, uint16_t port,
                          const HostId& localHostId, const std::string& localHostNameUtf8,
                          const HostId& expectedHostId,
                          std::chrono::milliseconds connectTimeout,
                          std::chrono::milliseconds sessionDeadline) {
    socket_ = ConnectTcpSocket(ip, port, connectTimeout);
    if (socket_ == INVALID_SOCKET) {
        g_logger.log(__FUNCTION__, Logger::Level::Debug, L"OneShotPeer: connect to %hs:%hu failed.", ip.c_str(), port);
        return false;
    }

    if (!wakeEvent_.Initialize()) {
        g_logger.log(__FUNCTION__, Logger::Level::Warning, L"OneShotPeer: failed to init wake socket.");
        return false;  // destructor closes socket_
    }

    // Watchdog covers handshake + the caller's whole exchange.
    deadlineThread_ = std::thread([this, sessionDeadline]() {
        std::unique_lock<std::mutex> lock(deadlineMutex_);
        if (!deadlineCV_.wait_for(lock, sessionDeadline, [this]() { return finished_; })) {
            stopRequested_.store(true);
            wakeEvent_.Signal();
        }
    });

    SocketIoContext io{ socket_, wakeEvent_, stopRequested_ };
    std::string remoteHostNameUtf8;
    if (!channel_.ClientHandshake(io, localHostId, localHostNameUtf8, remoteHostId_, remoteHostNameUtf8)) {
        g_logger.log(__FUNCTION__, Logger::Level::Debug, L"OneShotPeer: handshake with %hs:%hu failed.", ip.c_str(), port);
        return false;
    }

    if (remoteHostId_ != expectedHostId) {
        g_logger.log(__FUNCTION__, Logger::Level::Warning, L"OneShotPeer: identity mismatch for %hs:%hu.", ip.c_str(), port);
        return false;
    }

    connected_ = true;
    return true;
}

bool OneShotPeer::SendClipboardPayload(const ClipboardPayload& payload) {
    if (!connected_) return false;
    SocketIoContext io{ socket_, wakeEvent_, stopRequested_ };
    return ClipboardWire::SendClipboardPayload(channel_, io, payload);
}

bool OneShotPeer::SendFrame(const char* tag4, const unsigned char* body, uint32_t bodySize) {
    if (!connected_) return false;
    SocketIoContext io{ socket_, wakeEvent_, stopRequested_ };
    return channel_.SendFrame(io, tag4, body, bodySize);
}

bool OneShotPeer::RecvFrame(std::vector<unsigned char>& out) {
    if (!connected_) return false;
    SocketIoContext io{ socket_, wakeEvent_, stopRequested_ };
    return channel_.RecvFrame(io, out);
}

void OneShotPeer::Teardown() {
    {
        std::lock_guard<std::mutex> lock(deadlineMutex_);
        finished_ = true;
    }
    deadlineCV_.notify_one();
    if (deadlineThread_.joinable()) {
        deadlineThread_.join();
    }
    if (socket_ != INVALID_SOCKET) {
        shutdown(socket_, SD_BOTH);
        closesocket(socket_);
        socket_ = INVALID_SOCKET;
    }
    wakeEvent_.Close();
}
