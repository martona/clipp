#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>
#include "HostId.h"

class Settings {
public:
    static constexpr int DefaultTcpPort = 15353;
    static constexpr const char* DefaultListenerIp = "0.0.0.0";
    static constexpr uint64_t UnlimitedClipboardHistoryLimit = 0;
    static constexpr uint64_t DefaultClipboardHistoryMemoryLimitBytes = 256ull * 1024ull * 1024ull;
    static constexpr uint64_t DefaultClipboardHistoryMaxAgeSeconds = 24ull * 60ull * 60ull;
    static constexpr uint64_t DefaultClipboardHistoryMaxItems = 1000;
    static constexpr uint64_t DefaultClipboardSyncMaxItems = 30;
    // How many sequence numbers to reserve ahead of the in-memory counter and
    // persist on every flush. A crash loses at most this many numbers; the next
    // session resumes above them, avoiding any collision with the prior session.
    static constexpr uint64_t OriginSequenceBatchSize = 500;
    // Named-register policy. Stored-only (no GUI in v1): read from storage if a
    // power user set them, else these defaults. TTL is the idle expiry; the cap is
    // a write-time refusal at origin.
    static constexpr uint64_t DefaultRegisterTtlSeconds = 90ull * 24 * 60 * 60;  // 90 days
    static constexpr uint64_t DefaultRegisterMaxCount = 1024;
    // The register HLC floor is persisted reserved-ahead by this many ms (the
    // analogue of OriginSequenceBatchSize): a Settings write happens at most once
    // per this much wall-time progress, and a restart resumes strictly above the
    // last emission. See noteRegisterHlcWallMs.
    static constexpr uint64_t RegisterHlcFloorBatchMs = 60ull * 1000;  // 1 minute
    // Default for honorExternalPrivacyMarkers: respect "don't sync" markers
    // set by other apps (e.g. Chrome / password managers) on the OS clipboard.
    static constexpr bool DefaultHonorExternalPrivacyMarkers = true;
    // Default for maskShortTextPreviews: mask activity-list previews of short
    // single-token text (the might-be-a-password heuristic). Display-only —
    // never affects what syncs.
    static constexpr bool DefaultMaskShortTextPreviews = true;
    // Default for animateFlowFeedback: nudge the tray / menu bar icon when a
    // clipboard item is sent to or received from the group. Display-only; the
    // last-event tooltip / menu line stays available either way.
    static constexpr bool DefaultAnimateFlowFeedback = true;
    // The popup shows its "Arrow keys to select, Enter to paste" hint toast on
    // each of the first this-many summons (dismissed early by the first
    // action), then never again.
    static constexpr uint64_t PopupHintMaxShows = 5;

    // Popup summon hotkeys, PLATFORM-NATIVE chords: (modifiers << 16) | key.
    // Windows: RegisterHotKey MOD_* bits + virtual-key code. macOS: Carbon
    // cmdKey/shiftKey/optionKey/controlKey bits + Carbon keycode. 0 = slot
    // explicitly disabled; an ABSENT stored value means the default below
    // (settings never roam between devices, so native encoding is safe).
    // Two slots so an Insert-less Mac laptop keeps a typable chord while a
    // desk with a full keyboard gets both.
#if defined(_WIN32)
    static constexpr uint32_t DefaultPopupHotkeyPrimary = (0x8u << 16) | 0x2D;    // Win+Insert
    static constexpr uint32_t DefaultPopupHotkeySecondary = (0xAu << 16) | 0x56;  // Ctrl+Win+V
#elif defined(__APPLE__)
    static constexpr uint32_t DefaultPopupHotkeyPrimary = (0x0100u << 16) | 0x72;    // Cmd+Insert(Help)
    static constexpr uint32_t DefaultPopupHotkeySecondary = (0x1100u << 16) | 0x09;  // Ctrl+Cmd+V
#else
    static constexpr uint32_t DefaultPopupHotkeyPrimary = 0;
    static constexpr uint32_t DefaultPopupHotkeySecondary = 0;
#endif

    Settings();

    static bool IsValidPort(int value);
    static bool IsValidListenerIp(const std::string& value);

	std::string listenerIp() const;
    int tcpPort() const;
    std::string networkName() const;
    uint64_t clipboardHistoryMemoryLimitBytes() const;
    uint64_t clipboardHistoryMaxAgeSeconds() const;
    uint64_t clipboardHistoryMaxItems() const;
    uint64_t clipboardSyncMaxItems() const;
    bool honorExternalPrivacyMarkers() const;
    bool maskShortTextPreviews() const;
    bool animateFlowFeedback() const;

	bool set_listenerIp(const std::string& value);
    bool set_tcpPort(int value);
    bool set_networkName(const std::string& value);
    bool set_clipboardHistoryMemoryLimitBytes(uint64_t value);
    bool set_clipboardHistoryMaxAgeSeconds(uint64_t value);
    bool set_clipboardHistoryMaxItems(uint64_t value);
    bool set_clipboardSyncMaxItems(uint64_t value);
    bool set_honorExternalPrivacyMarkers(bool value);
    bool set_maskShortTextPreviews(bool value);
    bool set_animateFlowFeedback(bool value);

    // Atomically increments the per-origin sequence counter and returns the next
    // value. Persists every OriginSequenceBatchSize calls. On startup the counter
    // is pre-bumped by one batch so an unclean shutdown never collides with the
    // next session. Counter is per-origin (this device), monotonic across restarts.
    uint64_t nextOriginSequenceNumber();

    // How many times the popup's first-run hint toast has been shown. Bumped
    // (and persisted) once per summon that shows it; the popup stops showing
    // the toast at PopupHintMaxShows.
    uint64_t popupHintShownCount() const;
    void notePopupHintShown();

    // The Clipboard page's teach-the-hotkey banner: dismissed forever once the
    // user closes it (no un-dismiss UI; by design).
    bool popupTeachBannerDismissed() const;
    void notePopupTeachBannerDismissed();

    // The two popup summon chords (see the Default* constants for encoding).
    // Setters persist immediately; callers re-register via the platform's
    // ReapplyPopupHotkeys after a change.
    uint32_t popupHotkeyPrimary() const;
    uint32_t popupHotkeySecondary() const;
    void setPopupHotkeyPrimary(uint32_t chord);
    void setPopupHotkeySecondary(uint32_t chord);

    // Named-register settings (stored-only). TTL in seconds; cap in records.
    uint64_t registerTtlSeconds() const;
    uint64_t registerMaxCount() const;
    // Persisted HLC high-water (wall ms) so the register clock never regresses
    // across restarts — the mesh is the durable store, so a regressed local clock
    // could otherwise let a fresh write silently lose the LWW compare. Reserved-
    // ahead and batched, mirroring nextOriginSequenceNumber's floor discipline.
    uint64_t registerHlcFloorMs() const;
    void noteRegisterHlcWallMs(uint64_t wallMs);

    bool setNetworkKey(const std::vector<unsigned char>& value);
    bool getNetworkKey(std::vector<unsigned char>& value) const;
    bool ensureHostID(HostId& value);
    bool getHostID(HostId& value) const;
    bool resetHostID(HostId& value);

private:
    bool LoadCache();
    static bool ReadStringValue(const wchar_t* valueName, std::string& outValue);
    static bool ReadUint32Value(const wchar_t* valueName, int& outValue);
    static bool ReadUint64Value(const wchar_t* valueName, uint64_t& outValue);
    static bool WriteStringValue(const wchar_t* valueName, const std::string& value);
    static bool WriteUint32Value(const wchar_t* valueName, int value);
    static bool WriteUint64Value(const wchar_t* valueName, uint64_t value);
    static bool WriteBinaryValue(const wchar_t* valueName, const unsigned char* data, size_t len);
    static bool ReadBinaryValue(const wchar_t* valueName, std::vector<unsigned char>& outValue);
    static std::string GetDefaultNetworkName();

	std::string listenerIp_;
    int tcpPort_;
    std::string networkName_;
    uint64_t clipboardHistoryMemoryLimitBytes_;
    uint64_t clipboardHistoryMaxAgeSeconds_;
    uint64_t clipboardHistoryMaxItems_;
    uint64_t clipboardSyncMaxItems_;
    bool honorExternalPrivacyMarkers_;
    bool maskShortTextPreviews_;
    bool animateFlowFeedback_;
    // In-memory origin sequence counter. Highest value yielded so far.
    uint64_t originSequenceCounter_{ 0 };
    // The next persisted floor — counter values up to (but not including) this
    // are safe to mint without a write. When counter reaches this, we bump the
    // floor by OriginSequenceBatchSize and persist.
    uint64_t originSequencePersistedFloor_{ 0 };
    uint64_t registerTtlSeconds_;
    uint64_t registerMaxCount_;
    // Reserved-ahead persisted HLC wall-ms floor for the register clock.
    uint64_t registerHlcFloorMs_{ 0 };
    uint64_t popupHintShownCount_{ 0 };
    bool popupTeachBannerDismissed_{ false };
    uint32_t popupHotkeyPrimary_{ DefaultPopupHotkeyPrimary };
    uint32_t popupHotkeySecondary_{ DefaultPopupHotkeySecondary };
    mutable std::mutex mutex_;
};

extern Settings g_settings;
