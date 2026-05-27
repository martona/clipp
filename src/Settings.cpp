#include "platform.h"
#include <cstdint>
#include <cstring>
#include <sodium.h>
#include "Settings.h"

namespace {
    constexpr wchar_t kMulticastIpName[] = L"MulticastIp";
	constexpr wchar_t kListenerIpName[] = L"ListenerIp";
    constexpr wchar_t kMdnsPortName[] = L"MdnsPort";
    constexpr wchar_t kTcpPortName[] = L"TcpPort";
    constexpr wchar_t kNetworkNameName[] = L"NetworkName";
    constexpr wchar_t kClipboardHistoryMemoryLimitBytesName[] = L"ClipboardHistoryMemoryLimitBytes";
    constexpr wchar_t kClipboardHistoryMaxAgeSecondsName[] = L"ClipboardHistoryMaxAgeSeconds";
    constexpr wchar_t kClipboardHistoryMaxItemsName[] = L"ClipboardHistoryMaxItems";
    constexpr wchar_t kClipboardSyncMaxItemsName[] = L"ClipboardSyncMaxItems";
    constexpr wchar_t kHonorExternalPrivacyMarkersName[] = L"HonorExternalPrivacyMarkers";
    constexpr wchar_t kOriginSequenceFloorName[] = L"OriginSequenceFloor";
    constexpr wchar_t kEncryptedNetworkKeyName[] = L"EncryptedNetworkKey";
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

bool Settings::IsValidMulticastIp(const std::string& value) {
    if (value.empty() || value.size() > 15) {
        return false;
    }

    in_addr address{};
    if (inet_pton(AF_INET, value.c_str(), &address) != 1) {
        return false;
    }

    const uint32_t hostOrder = ntohl(address.s_addr);
    return hostOrder >= 0xe0000000u && hostOrder <= 0xefffffffu;
}

Settings::Settings()
    : multicastIp_(DefaultMulticastIp),
      listenerIp_(DefaultListenerIp),
      mdnsPort_(DefaultMdnsPort),
      tcpPort_(DefaultTcpPort),
      networkName_(GetDefaultNetworkName()),
      clipboardHistoryMemoryLimitBytes_(DefaultClipboardHistoryMemoryLimitBytes),
      clipboardHistoryMaxAgeSeconds_(DefaultClipboardHistoryMaxAgeSeconds),
      clipboardHistoryMaxItems_(DefaultClipboardHistoryMaxItems),
      clipboardSyncMaxItems_(DefaultClipboardSyncMaxItems),
      honorExternalPrivacyMarkers_(DefaultHonorExternalPrivacyMarkers) {
    LoadCache();
}

std::string Settings::multicastIp() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return multicastIp_;
}

std::string Settings::listenerIp() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return listenerIp_;
}

int Settings::mdnsPort() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return mdnsPort_;
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

bool Settings::set_multicastIp(const std::string& value) {
    if (!IsValidMulticastIp(value)) {
        return false;
    }
    if (!WriteStringValue(kMulticastIpName, value)) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    multicastIp_ = value;
    return true;
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

bool Settings::set_mdnsPort(int value) {
    if (!IsValidPort(value)) {
        return false;
    }
    if (!WriteUint32Value(kMdnsPortName, value)) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    mdnsPort_ = value;
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

bool Settings::setEncryptedNetworkKey(const std::vector<unsigned char>& value) {
    return WriteBinaryValue(kEncryptedNetworkKeyName, value.data(), value.size());
}

bool Settings::getEncryptedNetworkKey(std::vector<unsigned char>& value) const {
    return ReadBinaryValue(kEncryptedNetworkKeyName, value);
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
    std::string multicast;
    std::string networkName;
    std::string ip;
    int mdns = DefaultMdnsPort;
    int tcp = DefaultTcpPort;
    uint64_t clipboardHistoryMemoryLimitBytes = DefaultClipboardHistoryMemoryLimitBytes;
    uint64_t clipboardHistoryMaxAgeSeconds = DefaultClipboardHistoryMaxAgeSeconds;
    uint64_t clipboardHistoryMaxItems = DefaultClipboardHistoryMaxItems;
    uint64_t clipboardSyncMaxItems = DefaultClipboardSyncMaxItems;
    int honorExternalPrivacyMarkers = DefaultHonorExternalPrivacyMarkers ? 1 : 0;
    uint64_t originSequenceFloor = 0;

    if (ReadStringValue(kListenerIpName, ip) && IsValidListenerIp(ip)) {
        listenerIp_ = ip;
    }
    if (ReadStringValue(kMulticastIpName, multicast) && IsValidMulticastIp(multicast)) {
        multicastIp_ = multicast;
    }
    if (ReadStringValue(kNetworkNameName, networkName)) {
        networkName_ = networkName;
    }
    if (ReadUint32Value(kMdnsPortName, mdns) && IsValidPort(mdns)) {
        mdnsPort_ = mdns;
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

    // Origin sequence counter: load the persisted floor (the value the previous
    // session reserved through). Start the in-memory counter from there so we
    // never reissue numbers from a crashed session.
    ReadUint64Value(kOriginSequenceFloorName, originSequenceFloor);
    originSequenceCounter_ = originSequenceFloor;
    originSequencePersistedFloor_ = originSequenceFloor;

    return true;
}
