#include "platform.h"
#include <cstring>
#include <sodium.h>
#include "Settings.h"

namespace {
    constexpr wchar_t kMulticastIpName[] = L"MulticastIp";
	constexpr wchar_t kListenerIpName[] = L"ListenerIp";
    constexpr wchar_t kMdnsPortName[] = L"MdnsPort";
    constexpr wchar_t kTcpPortName[] = L"TcpPort";
    constexpr wchar_t kNetworkNameName[] = L"NetworkName";
	constexpr wchar_t kNetworkNameTimestampName[] = L"NetworkNameTimestamp";
    constexpr wchar_t kEncryptedNetworkKeyName[] = L"EncryptedNetworkKey";
    constexpr wchar_t kHostIDName[] = L"HostID";
}

Settings::Settings()
    : multicastIp_(DefaultMulticastIp),
	  listenerIp_(DefaultListenerIp),
      mdnsPort_(DefaultMdnsPort),
      tcpPort_(DefaultTcpPort),
	  networkNameTimestamp_(0),
      networkName_(DefaultNetworkName) {
    LoadCache();
}

const std::string& Settings::multicastIp() const { return multicastIp_; }
const std::string& Settings::listenerIp() const { return listenerIp_; }
int Settings::mdnsPort() const { return mdnsPort_; }
int Settings::tcpPort() const { return tcpPort_; }
const std::string& Settings::networkName() const { return networkName_; }
const uint64_t Settings::networkNameTimestamp() const { return networkNameTimestamp_; }

bool Settings::set_multicastIp(const std::string& value) {
    if (!WriteStringValue(kMulticastIpName, value)) {
        return false;
    }
    multicastIp_ = value;
    return true;
}

bool Settings::set_listenerIp(const std::string& value) {
    if (!WriteStringValue(kListenerIpName, value)) {
        return false;
    }
    listenerIp_ = value;
    return true;
}

bool Settings::set_mdnsPort(int value) {
    if (!WriteUint32Value(kMdnsPortName, value)) {
        return false;
    }
    mdnsPort_ = value;
    return true;
}

bool Settings::set_tcpPort(int value) {
    if (!WriteUint32Value(kTcpPortName, value)) {
        return false;
    }
    tcpPort_ = value;
    return true;
}

bool Settings::set_networkName(const std::string& value) {
    std::string truncatedValue = value.substr(0, MaxNetworkNameLength);
    if (!WriteStringValue(kNetworkNameName, truncatedValue)) {
        return false;
    }
    networkName_ = truncatedValue;
    return true;
}

bool Settings::set_networkNameTimestamp(uint64_t value) {
    if (!WriteUint64Value(kNetworkNameTimestampName, value)) {
        return false;
    }
    networkNameTimestamp_ = value;
    return true;
}

bool Settings::setEncryptedNetworkKey(const std::vector<unsigned char>& value) {
    return WriteBinaryValue(kEncryptedNetworkKeyName, value.data(), value.size());
}

bool Settings::getEncryptedNetworkKey(std::vector<unsigned char>& value) const {
    return ReadBinaryValue(kEncryptedNetworkKeyName, value);
}

bool Settings::ensureHostID(std::array<unsigned char, 32>& value) {
    std::vector<unsigned char> existingValue;
    if (ReadBinaryValue(kHostIDName, existingValue) && existingValue.size() == value.size()) {
        std::memcpy(value.data(), existingValue.data(), value.size());
        return true;
    }

    randombytes_buf(value.data(), value.size());

    return WriteBinaryValue(kHostIDName, value.data(), value.size());
}

bool Settings::getHostID(std::array<unsigned char, 32>& value) const {
    std::vector<unsigned char> hostID;
    if (!ReadBinaryValue(kHostIDName, hostID) || hostID.size() != value.size()) {
        return false;
    }

    std::memcpy(value.data(), hostID.data(), value.size());
    return true;
}

bool Settings::LoadCache() {
    std::string multicast;
    std::string networkName;
    int mdns = DefaultMdnsPort;
    int tcp = DefaultTcpPort;
	uint64_t networkNameTimestamp = 0;

    if (ReadStringValue(kMulticastIpName, multicast) && !multicast.empty()) {
        multicastIp_ = multicast;
    }
    if (ReadStringValue(kNetworkNameName, networkName)) {
        networkName_ = networkName.substr(0, MaxNetworkNameLength);
    }
    if (ReadUint64Value(kNetworkNameTimestampName, networkNameTimestamp)) {
        networkNameTimestamp_ = networkNameTimestamp;
	}
    if (ReadUint32Value(kMdnsPortName, mdns)) {
        mdnsPort_ = mdns;
    }
    if (ReadUint32Value(kTcpPortName, tcp)) {
        tcpPort_ = tcp;
    }

    return true;
}

