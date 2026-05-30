// Linux settings backend.
//
// Mirrors the Windows registry backend (platform/win32/Settings.cpp) and the macOS
// NSUserDefaults backend (platform/macos/Settings.mm): it implements ONLY the
// static Read*/Write*Value helpers + GetDefaultNetworkName. The portable Settings
// logic (ctor, getters/setters, LoadCache, host-id, sequence counter) lives in
// src/Settings.cpp and is shared across all platforms.
//
// Storage: a single key=value text file under $XDG_CONFIG_HOME/clipp (or
// ~/.config/clipp), created 0600. The model is stateless like the registry backend
// -- every read parses the whole file; every write parses-mutates-rewrites it, all
// under one file-scope mutex, with the rewrite made atomic via temp-file + rename().
//
// The network key is persisted here too (see KeyManager's Linux branch), hence
// 0600: same trust model as an ~/.ssh private key, with full-disk encryption
// covering data at rest. We deliberately do NOT use libsecret -- a headless SSH
// session typically has no unlocked Secret Service / D-Bus session, which is
// exactly the environment this build targets.

#include "platform.h"
#include "Settings.h"
#include "utils.h"

#include <sodium.h>

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include <fcntl.h>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace {

// Serializes all file access. The Settings instance has its own mutex for the
// in-memory cache; this independent lock guards the on-disk file. nextOrigin-
// SequenceNumber holds the instance mutex then calls Write* (which takes this one):
// the order is always instance -> file, never the reverse, so no deadlock.
std::mutex g_fileMutex;

std::string HomeDir() {
    if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
        return home;
    }
    if (struct passwd* pw = getpwuid(getuid())) {
        if (pw->pw_dir != nullptr && pw->pw_dir[0] != '\0') {
            return pw->pw_dir;
        }
    }
    return {};
}

std::string ConfigDir() {
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg != nullptr && xdg[0] != '\0') {
        return std::string(xdg) + "/clipp";
    }
    const std::string home = HomeDir();
    if (!home.empty()) {
        return home + "/.config/clipp";
    }
    return "./clipp";  // last resort: cwd-relative
}

std::string ConfigFilePath() {
    return ConfigDir() + "/settings";
}

// mkdir -p the config directory (mode 0700). Returns true if it exists afterward.
bool EnsureConfigDir() {
    const std::string dir = ConfigDir();
    std::string partial;
    for (size_t i = 0; i < dir.size(); ++i) {
        partial.push_back(dir[i]);
        const bool atEnd = (i + 1 == dir.size());
        if (dir[i] == '/' || atEnd) {
            if (partial == "/") {
                continue;  // root always exists; mkdir("/") would just EEXIST
            }
            if (mkdir(partial.c_str(), 0700) != 0 && errno != EEXIST) {
                return false;
            }
        }
    }
    return true;
}

// Parse the file into a key->value map. A missing file yields an empty map (not an
// error -- it's the first-run / no-settings-yet state).
std::map<std::string, std::string> LoadAll() {
    std::map<std::string, std::string> values;
    std::ifstream in(ConfigFilePath(), std::ios::binary);
    if (!in.is_open()) {
        return values;
    }
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();  // tolerate CRLF
        }
        if (line.empty()) {
            continue;
        }
        const size_t eq = line.find('=');
        if (eq == std::string::npos) {
            continue;  // malformed line; skip
        }
        // Split on the FIRST '=' so values may themselves contain '='.
        values[line.substr(0, eq)] = line.substr(eq + 1);
    }
    return values;
}

// Serialize + atomically replace the file (temp + fsync + rename), mode 0600.
bool StoreAll(const std::map<std::string, std::string>& values) {
    if (!EnsureConfigDir()) {
        return false;
    }
    const std::string path = ConfigFilePath();
    const std::string tmp = path + ".tmp";

    std::string content;
    for (const auto& [key, value] : values) {
        content += key;
        content.push_back('=');
        content += value;
        content.push_back('\n');
    }

    const int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        return false;
    }
    size_t offset = 0;
    while (offset < content.size()) {
        const ssize_t written = write(fd, content.data() + offset, content.size() - offset);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            close(fd);
            unlink(tmp.c_str());
            return false;
        }
        offset += static_cast<size_t>(written);
    }
    fsync(fd);
    close(fd);
    if (rename(tmp.c_str(), path.c_str()) != 0) {
        unlink(tmp.c_str());
        return false;
    }
    return true;
}

// The value-name keys are wide (e.g. L"TcpPort") for parity with the Win32 registry
// API. They're pure ASCII, so this is a trivial widening->UTF-8; used as the file key.
std::string KeyName(const wchar_t* valueName) {
    return WideToUtf8String(valueName != nullptr ? valueName : L"");
}

std::string Base64Encode(const unsigned char* data, size_t len) {
    if (data == nullptr || len == 0) {
        return {};
    }
    // VARIANT_ORIGINAL (with padding): this is a config file, not a DNS TXT record,
    // so the unpadded convention used on the wire (MDNSProtocol) does not apply.
    const size_t encodedLen = sodium_base64_encoded_len(len, sodium_base64_VARIANT_ORIGINAL);
    std::string out(encodedLen, '\0');
    sodium_bin2base64(out.data(), out.size(), data, len, sodium_base64_VARIANT_ORIGINAL);
    if (!out.empty() && out.back() == '\0') {
        out.pop_back();  // encoded_len counts the trailing NUL
    }
    return out;
}

bool Base64Decode(const std::string& text, std::vector<unsigned char>& out) {
    out.assign(text.size(), 0);  // decoded length is always <= input length
    size_t decodedLen = 0;
    const int rc = sodium_base642bin(
        out.data(), out.size(),
        text.data(), text.size(),
        nullptr, &decodedLen, nullptr,
        sodium_base64_VARIANT_ORIGINAL);
    if (rc != 0) {
        out.clear();
        return false;
    }
    out.resize(decodedLen);
    return true;
}

}  // namespace

bool Settings::ReadStringValue(const wchar_t* valueName, std::string& outValue) {
    std::lock_guard<std::mutex> lock(g_fileMutex);
    const auto values = LoadAll();
    const auto it = values.find(KeyName(valueName));
    if (it == values.end()) {
        return false;
    }
    outValue = it->second;
    return true;
}

bool Settings::ReadUint32Value(const wchar_t* valueName, int& outValue) {
    std::string text;
    if (!ReadStringValue(valueName, text)) {
        return false;
    }
    errno = 0;
    char* end = nullptr;
    const long parsed = std::strtol(text.c_str(), &end, 10);
    if (errno != 0 || end == text.c_str() || *end != '\0') {
        return false;
    }
    outValue = static_cast<int>(parsed);
    return true;
}

bool Settings::ReadUint64Value(const wchar_t* valueName, uint64_t& outValue) {
    std::string text;
    if (!ReadStringValue(valueName, text)) {
        return false;
    }
    errno = 0;
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(text.c_str(), &end, 10);
    if (errno != 0 || end == text.c_str() || *end != '\0') {
        return false;
    }
    outValue = static_cast<uint64_t>(parsed);
    return true;
}

bool Settings::WriteStringValue(const wchar_t* valueName, const std::string& value) {
    std::lock_guard<std::mutex> lock(g_fileMutex);
    auto values = LoadAll();
    values[KeyName(valueName)] = value;
    return StoreAll(values);
}

bool Settings::WriteUint32Value(const wchar_t* valueName, int value) {
    return WriteStringValue(valueName, std::to_string(value));
}

bool Settings::WriteUint64Value(const wchar_t* valueName, uint64_t value) {
    return WriteStringValue(valueName, std::to_string(value));
}

bool Settings::WriteBinaryValue(const wchar_t* valueName, const unsigned char* data, size_t len) {
    return WriteStringValue(valueName, Base64Encode(data, len));
}

bool Settings::ReadBinaryValue(const wchar_t* valueName, std::vector<unsigned char>& outValue) {
    std::string text;
    if (!ReadStringValue(valueName, text)) {
        return false;
    }
    if (text.empty()) {
        outValue.clear();  // present-but-empty (e.g. after ClearNetworkKey)
        return true;
    }
    return Base64Decode(text, outValue);
}

std::string Settings::GetDefaultNetworkName() {
    std::string username;
    if (const char* user = std::getenv("USER"); user != nullptr && user[0] != '\0') {
        username = user;
    }
    if (username.empty()) {
        if (struct passwd* pw = getpwuid(getuid())) {
            if (pw->pw_name != nullptr && pw->pw_name[0] != '\0') {
                username = pw->pw_name;
            }
        }
    }
    if (username.empty()) {
        username = "local";
    }
    return username + "'s clipp network";
}
