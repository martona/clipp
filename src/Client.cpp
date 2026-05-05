#include "Logger.h"
#include "Client.h"

#include "platform.h"

#include <iostream>
#include <cwchar>
#include <chrono>
#include <cstring>
#include <vector>

#include "CryptoChannel.h"
#include "NetworkDefs.h"


Client::Client(SOCKET socket, ClipboardReceivedCallback clipboardReceivedCallback)
    : clipboardReceivedCallback_(std::move(clipboardReceivedCallback)) {
	socket_ = socket;
}

Client::~Client() {
    Terminate();
}

void Client::Start() {
    running_.store(true);
    thread_ = std::thread(&Client::ThreadProc, this);
}

void Client::Terminate() {
    stopRequested_.store(true);
    {
        std::lock_guard<std::mutex> lock(socketMutex_);
        if (socket_ != INVALID_SOCKET) {
            shutdown(socket_, SD_BOTH);
            closesocket(socket_);
            socket_ = INVALID_SOCKET;
        }
    }

    if (thread_.joinable()) {
        thread_.join();
    }
}

bool Client::IsRunning() const {
	return running_.load();
}

std::array<unsigned char, 32> Client::remoteHostID() const {
    std::lock_guard<std::mutex> lock(remoteInfoMutex_);
    return remoteHostID_;
}

std::wstring Client::remoteHostName() const {
    std::lock_guard<std::mutex> lock(remoteInfoMutex_);
    return remoteHostName_;
}

bool Client::RecvAll(SOCKET sock, char* buffer, int length) {
    size_t total = 0;
    while (total < length) {
        size_t received = recv(sock, buffer + total, (int)(length - total), 0);
        if (received == 0) {
            return false;
        }
        total += received;
    }
    return true;
}

bool Client::SendAll(SOCKET sock, const char* buffer, int length) {
    size_t total = 0;
    while (total < length) {
        size_t sent = send(sock, buffer + total, (int)(length - total), 0);
        if (sent == 0) {
            return false;
        }
        total += sent;
    }
    return true;
}

void Client::ThreadProc() {
    CryptoChannel channel;
    std::array<unsigned char, 32> remoteHostId{};
    std::string remoteHostNameUtf8;
    if (!channel.ServerHandshake(socket_, remoteHostId, remoteHostNameUtf8)) {
        g_logger.log(__FUNCTION__, Logger::Level::Error, L"Client secure handshake failed.");
    } else {
        {
            std::lock_guard<std::mutex> lock(remoteInfoMutex_);
            remoteHostID_ = remoteHostId;
            size_t remoteHostNameWLen = utf8_to_utf16(remoteHostNameUtf8.c_str(), remoteHostNameUtf8.size(), nullptr, 0);
            std::wstring remoteHostName(remoteHostNameWLen > 0 ? remoteHostNameWLen - 1 : 0, L'\0');
            if (remoteHostNameWLen > 1) {
                utf8_to_utf16(remoteHostNameUtf8.c_str(), remoteHostNameUtf8.size(), remoteHostName.data(), remoteHostNameWLen);
            }
            remoteHostName_ = remoteHostName;
        }
        g_logger.log(__FUNCTION__, Logger::Level::Info, "Client connected: %s", remoteHostNameUtf8.c_str());

        char packet[4] = {};
        while (!stopRequested_.load()) {
            if (!channel.RecvTaggedMessage(socket_, packet)) {
                break;
            }

            if (std::memcmp(packet, "PING", 4) == 0) {
                g_logger.log(__FUNCTION__, Logger::Level::Debug, L"Client: PING");
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
                    g_logger.log(__FUNCTION__, Logger::Level::Warning, L"Rejecting clipboard message: header size mismatch (expected %zu bytes, actual %zu bytes)", sizeof(NetworkDefs::ClipboardMessage), headerMsg.size());
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
					g_logger.log(__FUNCTION__, Logger::Level::Warning, L"Rejecting clipboard message: encoded size mismatch (header: %u bytes, body: %zu bytes)", encodedDataSize, payload.rawData.size());
					break;
				}

				if (payload.decodedDataSize > ClipboardLimits::kMaxDecompressedClipboardBytes) {
                    g_logger.log(__FUNCTION__, Logger::Level::Warning, L"Rejecting clipboard message: decoded size %u bytes exceeds limit %llu bytes", payload.decodedDataSize, ClipboardLimits::kMaxDecompressedClipboardBytes);
					break;
				}

				if (!payload.isCompressed && encodedDataSize != payload.decodedDataSize) {
                    g_logger.log(__FUNCTION__, Logger::Level::Warning, L"Rejecting uncompressed clipboard message: encoded size %u bytes does not equal decoded size %u bytes", encodedDataSize, payload.decodedDataSize);
					break;
				}

				if (!payload.ZstdDecompress()) {
					break;
				}

				if (clipboardReceivedCallback_) {
                    std::array<unsigned char, 32> remoteHostId;
                    std::wstring remoteHostName;
                    {
                        std::lock_guard<std::mutex> lock(remoteInfoMutex_);
                        remoteHostId = remoteHostID_;
                        remoteHostName = remoteHostName_;
                    }
                    clipboardReceivedCallback_(remoteHostName, remoteHostId, payload);
                }
            }
        }
    }

    running_.store(false);

}
