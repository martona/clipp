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
      networkName_(GetDefaultNetworkName()) {
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
	uint64_t networkNameTimestamp = 0;

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

    return true;
}
