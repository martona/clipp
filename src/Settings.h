#pragma once

#include <array>
#include <cstddef>
#include <mutex>
#include <string>
#include <vector>
#include "HostId.h"

class Settings {
public:
    static constexpr int DefaultMdnsPort = 15353;
    static constexpr int DefaultTcpPort = 15353;
    static constexpr const char* DefaultMulticastIp = "239.255.10.10";
    static constexpr const char* DefaultListenerIp = "0.0.0.0";

    Settings();

    static bool IsValidPort(int value);
    static bool IsValidListenerIp(const std::string& value);
    static bool IsValidMulticastIp(const std::string& value);

    std::string multicastIp() const;
	std::string listenerIp() const;
    int mdnsPort() const;
    int tcpPort() const;
    std::string networkName() const;

    bool set_multicastIp(const std::string& value);
	bool set_listenerIp(const std::string& value);
    bool set_mdnsPort(int value);
    bool set_tcpPort(int value);
    bool set_networkName(const std::string& value);

    bool setEncryptedNetworkKey(const std::vector<unsigned char>& value);
    bool getEncryptedNetworkKey(std::vector<unsigned char>& value) const;
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

    std::string multicastIp_;
	std::string listenerIp_;
    int mdnsPort_;
    int tcpPort_;
    std::string networkName_;
    mutable std::mutex mutex_;
};

extern Settings g_settings;
