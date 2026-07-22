#include "platform.h"
#include <cstdint>
#include <cstring>
#include <sodium.h>
#include "Settings.h"

namespace {
	constexpr wchar_t kListenerIpName[] = L"ListenerIp";
    constexpr wchar_t kTcpPortName[] = L"TcpPort";
    constexpr wchar_t kNetworkNameName[] = L"NetworkName";
    constexpr wchar_t kClipboardHistoryMemoryLimitBytesName[] = L"ClipboardHistoryMemoryLimitBytes";
    constexpr wchar_t kClipboardHistoryMaxAgeSecondsName[] = L"ClipboardHistoryMaxAgeSeconds";
    constexpr wchar_t kClipboardHistoryMaxItemsName[] = L"ClipboardHistoryMaxItems";
    constexpr wchar_t kClipboardSyncMaxItemsName[] = L"ClipboardSyncMaxItems";
    constexpr wchar_t kHonorExternalPrivacyMarkersName[] = L"HonorExternalPrivacyMarkers";
    constexpr wchar_t kMaskShortTextPreviewsName[] = L"MaskShortTextPreviews";
    constexpr wchar_t kAnimateFlowFeedbackName[] = L"AnimateFlowFeedback";
    constexpr wchar_t kOriginSequenceFloorName[] = L"OriginSequenceFloor";
    constexpr wchar_t kRegisterTtlSecondsName[] = L"RegisterTtlSeconds";
    constexpr wchar_t kRegisterMaxCountName[] = L"RegisterMaxCount";
    constexpr wchar_t kRegisterHlcFloorMsName[] = L"RegisterHlcFloorMs";
    constexpr wchar_t kNetworkKeyName[] = L"NetworkKey";
    // Legacy name, read-migrated below. Originally "Encrypted..." because Windows
    // DPAPI-wraps the blob, but the value is plaintext on Linux (raw 0600 key), so
    // the name was a lie on that platform. macOS never used this (keychain), so only
    // existing Windows installs (and any pre-rename Linux key) carry the old name.
    constexpr wchar_t kLegacyEncryptedNetworkKeyName[] = L"EncryptedNetworkKey";
    constexpr wchar_t kHostIDName[] = L"HostID";

    void GenerateHostID(HostId& value) {
        randombytes_buf(value.data().data(), value.data().size());
    }
}

bool Settings::IsValidPort(int value) {
    return value >= 1 && value <= 65535;
}

bool Settings::IsValidListenerIp(const std::string& value) {
    if (value.empty() || value.size() > 15) {
        return false;
    }

    in_addr address{};
    return inet_pton(AF_INET, value.c_str(), &address) == 1;
}

Settings::Settings()
    : listenerIp_(DefaultListenerIp),
      tcpPort_(DefaultTcpPort),
      networkName_(GetDefaultNetworkName()),
      clipboardHistoryMemoryLimitBytes_(DefaultClipboardHistoryMemoryLimitBytes),
      clipboardHistoryMaxAgeSeconds_(DefaultClipboardHistoryMaxAgeSeconds),
      clipboardHistoryMaxItems_(DefaultClipboardHistoryMaxItems),
      clipboardSyncMaxItems_(DefaultClipboardSyncMaxItems),
      honorExternalPrivacyMarkers_(DefaultHonorExternalPrivacyMarkers),
      maskShortTextPreviews_(DefaultMaskShortTextPreviews),
      animateFlowFeedback_(DefaultAnimateFlowFeedback),
      registerTtlSeconds_(DefaultRegisterTtlSeconds),
      registerMaxCount_(DefaultRegisterMaxCount) {
    LoadCache();
}

std::string Settings::listenerIp() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return listenerIp_;
}

int Settings::tcpPort() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return tcpPort_;
}

std::string Settings::networkName() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return networkName_;
}

uint64_t Settings::clipboardHistoryMemoryLimitBytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return clipboardHistoryMemoryLimitBytes_;
}

uint64_t Settings::clipboardHistoryMaxAgeSeconds() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return clipboardHistoryMaxAgeSeconds_;
}

uint64_t Settings::clipboardHistoryMaxItems() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return clipboardHistoryMaxItems_;
}

uint64_t Settings::clipboardSyncMaxItems() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return clipboardSyncMaxItems_;
}

bool Settings::honorExternalPrivacyMarkers() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return honorExternalPrivacyMarkers_;
}

bool Settings::maskShortTextPreviews() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return maskShortTextPreviews_;
}

bool Settings::animateFlowFeedback() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return animateFlowFeedback_;
}

bool Settings::set_listenerIp(const std::string& value) {
    if (!IsValidListenerIp(value)) {
        return false;
    }
    if (!WriteStringValue(kListenerIpName, value)) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    listenerIp_ = value;
    return true;
}

bool Settings::set_tcpPort(int value) {
    if (!IsValidPort(value)) {
        return false;
    }
    if (!WriteUint32Value(kTcpPortName, value)) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    tcpPort_ = value;
    return true;
}

bool Settings::set_networkName(const std::string& value) {
    if (!WriteStringValue(kNetworkNameName, value)) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    networkName_ = value;
    return true;
}

bool Settings::set_clipboardHistoryMemoryLimitBytes(uint64_t value) {
    if (!WriteUint64Value(kClipboardHistoryMemoryLimitBytesName, value)) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    clipboardHistoryMemoryLimitBytes_ = value;
    return true;
}

bool Settings::set_clipboardHistoryMaxAgeSeconds(uint64_t value) {
    if (!WriteUint64Value(kClipboardHistoryMaxAgeSecondsName, value)) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    clipboardHistoryMaxAgeSeconds_ = value;
    return true;
}

bool Settings::set_clipboardHistoryMaxItems(uint64_t value) {
    if (!WriteUint64Value(kClipboardHistoryMaxItemsName, value)) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    clipboardHistoryMaxItems_ = value;
    return true;
}

bool Settings::set_clipboardSyncMaxItems(uint64_t value) {
    if (!WriteUint64Value(kClipboardSyncMaxItemsName, value)) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    clipboardSyncMaxItems_ = value;
    return true;
}

bool Settings::set_honorExternalPrivacyMarkers(bool value) {
    if (!WriteUint32Value(kHonorExternalPrivacyMarkersName, value ? 1 : 0)) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    honorExternalPrivacyMarkers_ = value;
    return true;
}

bool Settings::set_maskShortTextPreviews(bool value) {
    if (!WriteUint32Value(kMaskShortTextPreviewsName, value ? 1 : 0)) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    maskShortTextPreviews_ = value;
    return true;
}

bool Settings::set_animateFlowFeedback(bool value) {
    if (!WriteUint32Value(kAnimateFlowFeedbackName, value ? 1 : 0)) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    animateFlowFeedback_ = value;
    return true;
}

uint64_t Settings::nextOriginSequenceNumber() {
    std::lock_guard<std::mutex> lock(mutex_);
    const uint64_t next = ++originSequenceCounter_;
    if (next >= originSequencePersistedFloor_) {
        // Burn through one batch and persist the new ceiling. If we crash before
        // the next batch boundary we lose the unused tail, but the next session
        // starts above it — no collision.
        originSequencePersistedFloor_ = next + OriginSequenceBatchSize;
        WriteUint64Value(kOriginSequenceFloorName, originSequencePersistedFloor_);
    }
    return next;
}

uint64_t Settings::registerTtlSeconds() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return registerTtlSeconds_;
}

uint64_t Settings::registerMaxCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return registerMaxCount_;
}

uint64_t Settings::registerHlcFloorMs() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return registerHlcFloorMs_;
}

void Settings::noteRegisterHlcWallMs(uint64_t wallMs) {
    std::lock_guard<std::mutex> lock(mutex_);
    // Batched, reserved-ahead persist (mirrors nextOriginSequenceNumber): only
    // touch storage when the clock crosses the reserved floor, then reserve the
    // next block. A crash loses at most one block; the next session resumes above
    // it. Seeding the register clock to this floor on start prevents self-regression.
    if (wallMs >= registerHlcFloorMs_) {
        registerHlcFloorMs_ = wallMs + RegisterHlcFloorBatchMs;
        WriteUint64Value(kRegisterHlcFloorMsName, registerHlcFloorMs_);
    }
}

bool Settings::setNetworkKey(const std::vector<unsigned char>& value) {
    // Neutralize any value still stored under the legacy name, so it can't shadow
    // this write through getNetworkKey's migration fallback below. This matters for
    // the clear case: ClearNetworkKey writes an EMPTY value here, which the Windows
    // backend reads back as "absent" -- without also blanking the legacy, the next
    // read would fall through and resurrect the OLD key (erase would silently do
    // nothing). Blank (not delete -- the backend has no delete primitive) and only
    // when a non-empty legacy value is actually present, so fresh installs don't
    // accrue a vestigial empty legacy entry. An empty value reads as absent on
    // Windows and is never consulted on Linux (NetworkKey is present post-write),
    // so blanking is sufficient on every backend.
    std::vector<unsigned char> legacy;
    if (ReadBinaryValue(kLegacyEncryptedNetworkKeyName, legacy) && !legacy.empty()) {
        WriteBinaryValue(kLegacyEncryptedNetworkKeyName, legacy.data(), 0);
    }
    return WriteBinaryValue(kNetworkKeyName, value.data(), value.size());
}

bool Settings::getNetworkKey(std::vector<unsigned char>& value) const {
    if (ReadBinaryValue(kNetworkKeyName, value)) {
        return true;
    }
    // Migrate a key written under the legacy name by an older build: read it and
    // rewrite under the new name so the next read hits the fast path above. Reached
    // only when NetworkKey is genuinely absent (never written): once any value --
    // including the empty "cleared" tombstone -- exists under the new name, the
    // early return above wins and we never resurrect the legacy. setNetworkKey
    // additionally blanks the legacy on write, so a cleared key cannot come back.
    // One-time per install; spares existing users from re-entering credentials.
    if (ReadBinaryValue(kLegacyEncryptedNetworkKeyName, value)) {
        WriteBinaryValue(kNetworkKeyName, value.data(), value.size());
        return true;
    }
    return false;
}

bool Settings::ensureHostID(HostId& value) {
    std::vector<unsigned char> hostID;
    if (ReadBinaryValue(kHostIDName, hostID) && hostID.size() == HostId::kSize) {
		value.AssignFromVector(hostID);
        return true;
    }

    return resetHostID(value);
}

bool Settings::getHostID(HostId& value) const {
    std::vector<unsigned char> hostID;
    if (!ReadBinaryValue(kHostIDName, hostID) || hostID.size() != HostId::kSize) {
        return false;
    }

    return value.AssignFromVector(hostID);
}

bool Settings::resetHostID(HostId& value) {
    GenerateHostID(value);
    return WriteBinaryValue(kHostIDName, value.data().data(), value.data().size());
}

bool Settings::LoadCache() {
    std::string networkName;
    std::string ip;
    int tcp = DefaultTcpPort;
    uint64_t clipboardHistoryMemoryLimitBytes = DefaultClipboardHistoryMemoryLimitBytes;
    uint64_t clipboardHistoryMaxAgeSeconds = DefaultClipboardHistoryMaxAgeSeconds;
    uint64_t clipboardHistoryMaxItems = DefaultClipboardHistoryMaxItems;
    uint64_t clipboardSyncMaxItems = DefaultClipboardSyncMaxItems;
    int honorExternalPrivacyMarkers = DefaultHonorExternalPrivacyMarkers ? 1 : 0;
    int maskShortTextPreviews = DefaultMaskShortTextPreviews ? 1 : 0;
    int animateFlowFeedback = DefaultAnimateFlowFeedback ? 1 : 0;
    uint64_t originSequenceFloor = 0;

    if (ReadStringValue(kListenerIpName, ip) && IsValidListenerIp(ip)) {
        listenerIp_ = ip;
    }
    if (ReadStringValue(kNetworkNameName, networkName)) {
        networkName_ = networkName;
    }
    if (ReadUint32Value(kTcpPortName, tcp) && IsValidPort(tcp)) {
        tcpPort_ = tcp;
    }
    if (ReadUint64Value(kClipboardHistoryMemoryLimitBytesName, clipboardHistoryMemoryLimitBytes)) {
        clipboardHistoryMemoryLimitBytes_ = clipboardHistoryMemoryLimitBytes;
    }
    if (ReadUint64Value(kClipboardHistoryMaxAgeSecondsName, clipboardHistoryMaxAgeSeconds)) {
        clipboardHistoryMaxAgeSeconds_ = clipboardHistoryMaxAgeSeconds;
    }
    if (ReadUint64Value(kClipboardHistoryMaxItemsName, clipboardHistoryMaxItems)) {
        clipboardHistoryMaxItems_ = clipboardHistoryMaxItems;
    }
    if (ReadUint64Value(kClipboardSyncMaxItemsName, clipboardSyncMaxItems)) {
        clipboardSyncMaxItems_ = clipboardSyncMaxItems;
    }
    if (ReadUint32Value(kHonorExternalPrivacyMarkersName, honorExternalPrivacyMarkers)) {
        honorExternalPrivacyMarkers_ = (honorExternalPrivacyMarkers != 0);
    }
    if (ReadUint32Value(kMaskShortTextPreviewsName, maskShortTextPreviews)) {
        maskShortTextPreviews_ = (maskShortTextPreviews != 0);
    }
    if (ReadUint32Value(kAnimateFlowFeedbackName, animateFlowFeedback)) {
        animateFlowFeedback_ = (animateFlowFeedback != 0);
    }

    // Origin sequence counter: load the persisted floor (the value the previous
    // session reserved through). Start the in-memory counter from there so we
    // never reissue numbers from a crashed session.
    ReadUint64Value(kOriginSequenceFloorName, originSequenceFloor);
    originSequenceCounter_ = originSequenceFloor;
    originSequencePersistedFloor_ = originSequenceFloor;

    uint64_t registerTtlSeconds = DefaultRegisterTtlSeconds;
    uint64_t registerMaxCount = DefaultRegisterMaxCount;
    uint64_t registerHlcFloorMs = 0;
    if (ReadUint64Value(kRegisterTtlSecondsName, registerTtlSeconds)) {
        registerTtlSeconds_ = registerTtlSeconds;
    }
    if (ReadUint64Value(kRegisterMaxCountName, registerMaxCount)) {
        registerMaxCount_ = registerMaxCount;
    }
    ReadUint64Value(kRegisterHlcFloorMsName, registerHlcFloorMs);
    registerHlcFloorMs_ = registerHlcFloorMs;

    return true;
}
