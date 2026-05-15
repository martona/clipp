#pragma once
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <string>
#include "sodium.h"
#include "KeyManager.h"

class KeyDerivationWorker {
public:
    KeyDerivationWorker() {
        workerThread = std::thread(&KeyDerivationWorker::WorkerLoop, this);
    }

    ~KeyDerivationWorker() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            stopRequested = true;
        }
        cv.notify_one();
        if (workerThread.joinable()) {
            workerThread.join();
        }
    }

    // Called by UI Debounce timer
    void RequestKeyDerivation(const std::string& newPassword) {
        std::lock_guard<std::mutex> lock(mutex);
        sodium_memzero(pendingPassword.data(), pendingPassword.capacity());
        pendingPassword = newPassword;
        currentGeneration++; // Mark previous requests as stale
        hasPendingWork = true;
        cv.notify_one();     // Wake up the worker
    }

    struct KeyDerivationResult {
        std::array<unsigned char, KeyManager::NetworkKeySize> derivedKey;
		std::wstring derivedKeyHash;
	};

    void SetNotificationTarget(HWND hwnd, UINT messageId) {
        std::lock_guard<std::mutex> lock(mutex);
        notifyWindowHandle = hwnd;
        notifyMessageId = messageId;
    }

private:
    void WorkerLoop() {
        while (true) {
            std::string targetPassword;
            uint64_t targetGeneration = 0;

            // Wait for work
            {
                std::unique_lock<std::mutex> lock(mutex);
                cv.wait(lock, [this] { return hasPendingWork || stopRequested; });

                if (stopRequested) break;

                // Grab the latest password and its generation ID
                targetPassword = pendingPassword;
                targetGeneration = currentGeneration;
                sodium_memzero(pendingPassword.data(), pendingPassword.capacity());
                pendingPassword.clear();
                hasPendingWork = false;
            }

            // --- HEAVY LIFTING ---
            // This runs outside the lock, so the UI thread can still update 
            // the pendingPassword while this computes.
            std::array<unsigned char, KeyManager::NetworkKeySize> newKey{};
            bool success = g_keyManager.DeriveNetworkKey(targetPassword, newKey);
            sodium_memzero(targetPassword.data(), targetPassword.capacity());

            // --- CHECK IF STALE ---
            bool shouldApply = false;
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (targetGeneration == currentGeneration) {
                    // We are still the latest request! The user didn't type anything new.
                    shouldApply = success;
                } else {
                    // STALE: The user kept typing. Throw away this hash.
                    // The loop will immediately restart because hasPendingWork is true again.
                }
            }
            if (shouldApply) {
                // Apply the key to your PeerManager / Settings here
                ApplyKeyToSystem(newKey);
            }
        }
    }

    void ApplyKeyToSystem(const std::array<unsigned char, KeyManager::NetworkKeySize>& key) {
        HWND hwnd = nullptr;
        UINT messageId = 0;
        {
            std::lock_guard<std::mutex> lock(mutex);
            hwnd = notifyWindowHandle;
            messageId = notifyMessageId;
        }
		if (messageId == 0 || hwnd == nullptr) return;
		KeyDerivationResult result{};
        result.derivedKey = std::move(key);
        result.derivedKeyHash = std::move(g_keyManager.GetNetworkFingerprintHash(&key));
		SendMessage(hwnd, messageId, reinterpret_cast<WPARAM>(&result), 0);
    }

    std::thread workerThread;
    std::mutex mutex;
    std::condition_variable cv;

    bool stopRequested = false;
    bool hasPendingWork = false;
    std::string pendingPassword;
    uint64_t currentGeneration = 0;
	HWND notifyWindowHandle = nullptr; 
	UINT notifyMessageId = 0;
};
