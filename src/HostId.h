#pragma once
#include <array>
#include <cstring>
#include <string>
#include <vector>

class HostId {
public:
    HostId(const unsigned char* data) {
        std::memcpy(m_id.data(), data, m_id.size());
    }
    HostId(const std::array<unsigned char, 32>& data) : m_id(data) {}
	HostId() = default;
    ~HostId() = default;

    static constexpr size_t kSize = 32;

    bool AssignFromVector(std::vector<unsigned char>& data) {
        if (data.size() != kSize) {
            return false;
        }
        std::memcpy(m_id.data(), data.data(), m_id.size());
        return true;
	}

    bool operator==(const HostId& other) const {
        return std::memcmp(m_id.data(), other.m_id.data(), m_id.size()) == 0;
    }
    bool operator<(const HostId& other) const {
        return m_id < other.m_id;
    }
    bool operator>(const HostId& other) const {
        return m_id > other.m_id;
    }

    std::string ToHexString() const {
        return ToHexStringImpl<char>("0123456789abcdef");
    }
    std::wstring ToHexWString() const {
        return ToHexStringImpl<wchar_t>(L"0123456789abcdef");
    }

    const std::array<unsigned char, kSize>& data() const { return m_id; }
    std::array<unsigned char, kSize>& data() { return m_id; }

private:
    template<typename CharT>
    std::basic_string<CharT> ToHexStringImpl(const CharT* hexDigits) const {
        std::basic_string<CharT> hexString;
        hexString.reserve(m_id.size() * 2 + m_id.size() / 4);
        int idx = 0;
        for (unsigned char byte : m_id) {
            if (idx > 0 && idx % 4 == 0) {
                hexString.push_back(static_cast<CharT>('-'));
            }
            ++idx;
            hexString.push_back(hexDigits[byte >> 4]);
            hexString.push_back(hexDigits[byte & 0x0F]);
        }
        return hexString;
    }

    std::array<unsigned char, kSize> m_id{};
};