#pragma once

#include "platform.h"
#include <memory>
#include <mutex>
#include <vector>
#include <functional>
#include <array>
#include <string>

#include "ClipboardData.h"

class Client;

class ClientManager {
public:
    using ClipboardReceivedCallback = std::function<void(const std::wstring&, const std::array<unsigned char, 32>&, ClipboardPayload&)>;

    explicit ClientManager(ClipboardReceivedCallback callback = nullptr) : clipboardReceivedCallback_(std::move(callback)) {}

    void AddClient(SOCKET socket);
    void Cleanup();
    void Terminate();

private:
    std::mutex mutex_;
    ClipboardReceivedCallback clipboardReceivedCallback_;
    std::vector<std::unique_ptr<Client>> clients_;
};
