#pragma once
#include "platform.h"
#include "Settings.h"

namespace {
    constexpr wchar_t kRegistryPath[] = L"Software\\Clipp";
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
        size_t size_needed = utf16_to_utf8(buffer.c_str(), buffer.size(), nullptr, 0);
        outValue.resize(size_needed);
        utf16_to_utf8(buffer.c_str(), buffer.size(), &outValue[0], size_needed);
    }

    return true;
}

bool Settings::ReadUint32Value(const wchar_t* valueName, int& outValue) {
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

bool Settings::ReadUint64Value(const wchar_t* valueName, uint64_t& outValue) {
    HKEY keyHandle = nullptr;
    LONG status = RegOpenKeyExW(HKEY_CURRENT_USER, kRegistryPath, 0, KEY_QUERY_VALUE, &keyHandle);
    if (status != ERROR_SUCCESS) {
        return false;
    }

    DWORD type = 0;
    uint64_t value = 0;
    DWORD size = sizeof(uint64_t);
    status = RegQueryValueExW(keyHandle, valueName, nullptr, &type, reinterpret_cast<LPBYTE>(&value), &size);
    RegCloseKey(keyHandle);

    if (status != ERROR_SUCCESS || type != REG_DWORD || size != sizeof(uint64_t)) {
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

bool Settings::WriteUint32Value(const wchar_t* valueName, int value) {
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

bool Settings::WriteUint64Value(const wchar_t* valueName, uint64_t value) {
    HKEY keyHandle = nullptr;
    LONG status = RegCreateKeyExW(HKEY_CURRENT_USER, kRegistryPath, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &keyHandle, nullptr);
    if (status != ERROR_SUCCESS) {
        return false;
    }

    status = RegSetValueExW(keyHandle, valueName, 0, REG_QWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
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
