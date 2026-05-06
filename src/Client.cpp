#include "Logger.h"
#include "Client.h"

#include "platform.h"

#include <iostream>
#include <cwchar>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <vector>

#include "CryptoChannel.h"
#include "NetworkDefs.h"
#include "utils.h"

Client::Client(SOCKET socket, ClipboardReceivedCallback clipboardReceivedCallback)
    : clipboardReceivedCallback_(std::move(clipboardReceivedCallback)),
      ip_(Utf8ToWideString(SocketPeerIp(socket))),
      port_(SocketPeerPort(socket)),
      createdAt_(std::chrono::steady_clock::now()),
      lastPingReceivedAt_(createdAt_),
      socket_(socket) {
}

Client::~Client() {
    Stop();
}

void Client::Start() {
    running_.store(true);
    thread_ = std::thread(&Client::ThreadProc, this);
}

void Client::Stop() {
    stopRequested_.store(true);
    CloseSocket();

    if (thread_.joinable()) {
        thread_.join();
    }
}

bool Client::isRunning() const {
    return running_.load();
}

std::wstring Client::hostName() const {
    std::lock_guard<std::mutex> lock(dataMutex_);
    return hostName_;
}

std::array<unsigned char, 32> Client::hostID() const {
    std::lock_guard<std::mutex> lock(dataMutex_);
    return hostID_;
}

std::wstring Client::ip() const {
    std::lock_guard<std::mutex> lock(dataMutex_);
    return ip_;
}

unsigned short Client::port() const {
    std::lock_guard<std::mutex> lock(dataMutex_);
    return port_;
}

std::chrono::steady_clock::time_point Client::lastPingReceivedAt() const {
    std::lock_guard<std::mutex> lock(dataMutex_);
    return lastPingReceivedAt_;
}

std::chrono::steady_clock::time_point Client::createdAt() const {
    std::lock_guard<std::mutex> lock(dataMutex_);
    return createdAt_;
}

void Client::CloseSocket() {
    if (socket_ != INVALID_SOCKET) {
        shutdown(socket_, SD_BOTH);
        closesocket(socket_);
        socket_ = INVALID_SOCKET;
    }
}

void Client::log(const char* function, Logger::Level level, const wchar_t* message, ...) const {
    va_list args;
    va_start(args, message);
    logV(function, level, message, args);
    va_end(args);
}

void Client::logV(const char* function, Logger::Level level, const wchar_t* message, va_list args) const {
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

void Client::ThreadProc() {
    CryptoChannel channel;
    std::array<unsigned char, 32> remoteHostId{};
    std::string remoteHostNameUtf8;
    if (!channel.ServerHandshake(socket_, remoteHostId, remoteHostNameUtf8)) {
        log(__FUNCTION__, Logger::Level::Error, L"Client secure handshake failed.");
    } else {
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
