#pragma once

#include "../KeyManager.h"
#include "../Logger.h"
#include "../platform.h"
#include "uistrings.h"

#ifdef GetCurrentTime
#undef GetCurrentTime
#endif

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include <sodium.h>

namespace uiClippPage {
inline constexpr uint64_t kMaxByteCounter = 999'999'999'999;

inline std::string FormatByteCounter(uint64_t bytes) {
    if (bytes > kMaxByteCounter) {
        return "+++,+++,+++,+++";
    }

    std::string digits = std::to_string(bytes);
    std::string counter;
    counter.reserve(digits.size() + ((digits.size() - 1) / 3));
    for (std::size_t i = 0; i < digits.size(); ++i) {
        if (i > 0 && ((digits.size() - i) % 3) == 0) {
            counter.push_back(',');
        }
        counter.push_back(digits[i]);
    }
    return counter;
}

inline std::string FormatConnectionState(bool connected) {
    return connected ? CLP_UI_CONNECTED : CLP_UI_NOT_CONNECTED;
}

inline std::string FormatConnectedFor(
    std::chrono::steady_clock::time_point connectedSince,
    std::chrono::steady_clock::time_point now)
{
    if (connectedSince == std::chrono::steady_clock::time_point{}) {
        return CLP_UI_NOT_CONNECTED;
    }

    if (now < connectedSince) {
        now = connectedSince;
    }

    auto seconds = std::chrono::duration_cast<std::chrono::seconds>(now - connectedSince).count();
    const auto days = seconds / (60 * 60 * 24);
    seconds %= (60 * 60 * 24);
    const auto hours = seconds / (60 * 60);
    seconds %= (60 * 60);
    const auto minutes = seconds / 60;
    seconds %= 60;

    char timeBuffer[32]{};
    std::snprintf(timeBuffer,
                  sizeof(timeBuffer),
                  "%02lld:%02lld:%02lld",
                  static_cast<long long>(hours),
                  static_cast<long long>(minutes),
                  static_cast<long long>(seconds));

    std::string text = CLP_UI_CONNECTED_FOR;
    if (days > 0) {
        text += std::to_string(days);
        text += days == 1 ? CLP_UI_DAY_SUFFIX : CLP_UI_DAYS_SUFFIX;
    }
    text += timeBuffer;
    return text;
}

inline std::wstring DisplayHostNameOrUnknown(std::wstring_view hostName) {
    return hostName.empty() ? CLP_W(CLP_UI_UNKNOWN_HOST) : std::wstring(hostName);
}

class KeyDerivationWorker {
public:
    using ResultHandler = std::function<void(const KeyManager::NetworkKey&)>;

    explicit KeyDerivationWorker(ResultHandler resultHandler)
        : resultHandler_(std::move(resultHandler))
        , workerThread_(&KeyDerivationWorker::WorkerLoop, this) {
    }

    ~KeyDerivationWorker() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopRequested_ = true;
            sodium_memzero(pendingPassword_.data(), pendingPassword_.capacity());
            pendingPassword_.clear();
        }
        cv_.notify_one();
        if (workerThread_.joinable()) {
            workerThread_.join();
        }
    }

    KeyDerivationWorker(const KeyDerivationWorker&) = delete;
    KeyDerivationWorker& operator=(const KeyDerivationWorker&) = delete;

    void RequestKeyDerivation(const std::string& password) {
        std::lock_guard<std::mutex> lock(mutex_);
        sodium_memzero(pendingPassword_.data(), pendingPassword_.capacity());
        pendingPassword_ = password;
        ++currentGeneration_;
        hasPendingWork_ = true;
        cv_.notify_one();
    }

private:
    void WorkerLoop() {
        while (true) {
            std::string targetPassword;
            uint64_t targetGeneration = 0;

            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [this]() {
                    return hasPendingWork_ || stopRequested_;
                });

                if (stopRequested_) {
                    break;
                }

                targetPassword = pendingPassword_;
                targetGeneration = currentGeneration_;
                sodium_memzero(pendingPassword_.data(), pendingPassword_.capacity());
                pendingPassword_.clear();
                hasPendingWork_ = false;
            }

            KeyManager::NetworkKey newKey{};
            const bool success = g_keyManager.DeriveNetworkKey(targetPassword, newKey);
            sodium_memzero(targetPassword.data(), targetPassword.capacity());

            bool shouldApply = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                shouldApply = success && targetGeneration == currentGeneration_ && !stopRequested_;
            }

            if (shouldApply && resultHandler_) {
                resultHandler_(newKey);
            }
            sodium_memzero(newKey.data(), newKey.size());
        }
    }

    ResultHandler resultHandler_;
    std::thread workerThread_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool stopRequested_ = false;
    bool hasPendingWork_ = false;
    std::string pendingPassword_;
    uint64_t currentGeneration_ = 0;
};
}
