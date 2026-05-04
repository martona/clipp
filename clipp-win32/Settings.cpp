#include <windows.h>
#include <cstring>
#include <sodium.h>
#include "Settings.h"

namespace {
    constexpr wchar_t kRegistryPath[] = L"Software\\Clipp";
    constexpr wchar_t kMulticastIpName[] = L"MulticastIp";
	constexpr wchar_t kListenerIpName[] = L"ListenerIp";
    constexpr wchar_t kMdnsPortName[] = L"MdnsPort";
    constexpr wchar_t kTcpPortName[] = L"TcpPort";
    constexpr wchar_t kEncryptedNetworkKeyName[] = L"EncryptedNetworkKey";
    constexpr wchar_t kHostIDName[] = L"HostID";
}

Settings::Settings()
    : multicastIp_(DefaultMulticastIp),
	  listenerIp_(DefaultListenerIp),
      mdnsPort_(DefaultMdnsPort),
      tcpPort_(DefaultTcpPort) {
    LoadCache();
}

const std::string& Settings::multicastIp() const { return multicastIp_; }
const std::string& Settings::listenerIp() const { return listenerIp_; }
int Settings::mdnsPort() const { return mdnsPort_; }
int Settings::tcpPort() const { return tcpPort_; }

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
    if (!WriteDwordValue(kMdnsPortName, value)) {
        return false;
    }
    mdnsPort_ = value;
    return true;
}

bool Settings::set_tcpPort(int value) {
    if (!WriteDwordValue(kTcpPortName, value)) {
        return false;
    }
    tcpPort_ = value;
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
    int mdns = DefaultMdnsPort;
    int tcp = DefaultTcpPort;

    if (ReadStringValue(kMulticastIpName, multicast) && !multicast.empty()) {
        multicastIp_ = multicast;
    }
    if (ReadDwordValue(kMdnsPortName, mdns)) {
        mdnsPort_ = mdns;
    }
    if (ReadDwordValue(kTcpPortName, tcp)) {
        tcpPort_ = tcp;
    }

    return true;
}

bool Settings::ReadStringValue(const wchar_t* valueName, std::string& outValue) {
    HKEY keyHandle = nullptr;
    LONG status = RegOpenKeyExW(HKEY_CURRENT_USER, kRegistryPath, 0, KEY_QUERY_VALUE, &keyHandle);
    if (status != ERROR_SUCCESS) {
        return false;
    }

    DWORD type = 0;
    DWORD size = 0;
    status = RegQueryValueExW(keyHandle, valueName, nullptr, &type, nullptr, &size);
    if (status != ERROR_SUCCESS || type != REG_SZ || size == 0) {
        RegCloseKey(keyHandle);
        return false;
    }

    std::wstring buffer(size / sizeof(wchar_t), L'\0');
    status = RegQueryValueExW(keyHandle, valueName, nullptr, nullptr, reinterpret_cast<LPBYTE>(buffer.data()), &size);
    RegCloseKey(keyHandle);
    if (status != ERROR_SUCCESS) {
        return false;
    }

    if (!buffer.empty() && buffer.back() == L'\0') {
        buffer.pop_back();
    }

    if (!buffer.empty()) {
        int size_needed = WideCharToMultiByte(CP_UTF8, 0, buffer.c_str(), (int)buffer.size(), nullptr, 0, nullptr, nullptr);
        outValue.resize(size_needed);
        WideCharToMultiByte(CP_UTF8, 0, buffer.c_str(), (int)buffer.size(), &outValue[0], size_needed, nullptr, nullptr);
    }

    return true;
}

bool Settings::ReadDwordValue(const wchar_t* valueName, int& outValue) {
    HKEY keyHandle = nullptr;
    LONG status = RegOpenKeyExW(HKEY_CURRENT_USER, kRegistryPath, 0, KEY_QUERY_VALUE, &keyHandle);
    if (status != ERROR_SUCCESS) {
        return false;
    }

    DWORD type = 0;
    DWORD value = 0;
    DWORD size = sizeof(DWORD);
    status = RegQueryValueExW(keyHandle, valueName, nullptr, &type, reinterpret_cast<LPBYTE>(&value), &size);
    RegCloseKey(keyHandle);

    if (status != ERROR_SUCCESS || type != REG_DWORD || size != sizeof(DWORD)) {
        return false;
    }

    outValue = static_cast<int>(value);
    return true;
}

bool Settings::WriteStringValue(const wchar_t* valueName, const std::string& value) {
    std::wstring wideValue(value.begin(), value.end());

    HKEY keyHandle = nullptr;
    LONG status = RegCreateKeyExW(HKEY_CURRENT_USER, kRegistryPath, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &keyHandle, nullptr);
    if (status != ERROR_SUCCESS) {
        return false;
    }

    const DWORD size = static_cast<DWORD>((wideValue.size() + 1) * sizeof(wchar_t));
    status = RegSetValueExW(keyHandle, valueName, 0, REG_SZ, reinterpret_cast<const BYTE*>(wideValue.c_str()), size);
    RegCloseKey(keyHandle);
    return status == ERROR_SUCCESS;
}

bool Settings::WriteDwordValue(const wchar_t* valueName, int value) {
    HKEY keyHandle = nullptr;
    LONG status = RegCreateKeyExW(HKEY_CURRENT_USER, kRegistryPath, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &keyHandle, nullptr);
    if (status != ERROR_SUCCESS) {
        return false;
    }

    DWORD dwordValue = static_cast<DWORD>(value);
    status = RegSetValueExW(keyHandle, valueName, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&dwordValue), sizeof(dwordValue));
    RegCloseKey(keyHandle);
    return status == ERROR_SUCCESS;
}

bool Settings::WriteBinaryValue(const wchar_t* valueName, const unsigned char* data, size_t len) {
    HKEY keyHandle = nullptr;
    LONG status = RegCreateKeyExW(HKEY_CURRENT_USER, kRegistryPath, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &keyHandle, nullptr);
    if (status != ERROR_SUCCESS) {
        return false;
    }

    status = RegSetValueExW(keyHandle, valueName, 0, REG_BINARY, data, static_cast<DWORD>(len));
    RegCloseKey(keyHandle);
    return status == ERROR_SUCCESS;
}

bool Settings::ReadBinaryValue(const wchar_t* valueName, std::vector<unsigned char>& outValue) {
    HKEY keyHandle = nullptr;
    LONG status = RegOpenKeyExW(HKEY_CURRENT_USER, kRegistryPath, 0, KEY_QUERY_VALUE, &keyHandle);
    if (status != ERROR_SUCCESS) {
        return false;
    }

    DWORD type = 0;
    DWORD size = 0;
    status = RegQueryValueExW(keyHandle, valueName, nullptr, &type, nullptr, &size);
    if (status != ERROR_SUCCESS || type != REG_BINARY || size == 0) {
        RegCloseKey(keyHandle);
        return false;
    }

    outValue.resize(size);
    status = RegQueryValueExW(keyHandle, valueName, nullptr, nullptr, outValue.data(), &size);
    RegCloseKey(keyHandle);
    return status == ERROR_SUCCESS;
}
