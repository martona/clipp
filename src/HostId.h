#pragma once
#include <array>
#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

class HostId {
public:
    static constexpr size_t kSize = 16;
    using Bytes = std::array<unsigned char, kSize>;

    HostId(const unsigned char* data) {
        std::memcpy(m_id.data(), data, m_id.size());
    }
    HostId(const Bytes& data) : m_id(data) {}
	HostId() = default;
    ~HostId() = default;

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

    std::string ToHexString(size_t bytesPerGroup = 4) const {
        return ToHexStringImpl<char>("0123456789abcdef", bytesPerGroup);
    }
    std::wstring ToHexWString(size_t bytesPerGroup = 4) const {
        return ToHexStringImpl<wchar_t>(L"0123456789abcdef", bytesPerGroup);
    }

    const Bytes& data() const { return m_id; }
    Bytes& data() { return m_id; }

private:
    template<typename CharT>
    std::basic_string<CharT> ToHexStringImpl(const CharT* hexDigits, size_t bytesPerGroup) const {
        std::basic_string<CharT> hexString;
        hexString.reserve(m_id.size() * 2 + m_id.size() / 4);
        if (bytesPerGroup == 0) {
            bytesPerGroup = 4;
        }

        for (std::size_t idx = 0; idx < m_id.size(); ++idx) {
            if (idx > 0 && idx % bytesPerGroup == 0) {
                hexString.push_back(static_cast<CharT>('-'));
            }
            const unsigned char byte = m_id[idx];
            hexString.push_back(hexDigits[byte >> 4]);
            hexString.push_back(hexDigits[byte & 0x0F]);
        }
        return hexString;
    }

    Bytes m_id{};
};
