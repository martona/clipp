#pragma once

static bool RecvAll(SOCKET sock, char* buffer, int length) {
	long total = 0;
	while (total < length) {
		long received = recv(sock, buffer + total, (int)(length - total), 0);
		if (received <= 0) {
			return false;
		}
		total += received;
	}
	return true;
}

static bool SendAll(SOCKET sock, const char* buffer, int length) {
	long total = 0;
	while (total < length) {
		long sent = send(sock, buffer + total, (int)(length - total), 0);
		if (sent <= 0) {
			return false;
		}
		total += sent;
	}
	return true;
}

static std::wstring Utf8ToWideString(const std::string& value) {
	if (value.empty()) return L"";

	const size_t size = utf8_to_utf16(value.c_str(), value.size(), nullptr, 0);
	if (size == 0) return L"";

	std::wstring wide(size, L'\0');
	utf8_to_utf16(value.c_str(), value.size(), wide.data(), size);
	return wide;
}

static std::string WideToUtf8String(const std::wstring& value) {
	if (value.empty()) return "";

	const size_t size = utf16_to_utf8(value.c_str(), value.size(), nullptr, 0);
	if (size == 0) return "";

	std::string narrow(size, '\0');
	utf16_to_utf8(value.c_str(), value.size(), narrow.data(), size);
	return narrow;
}

static unsigned short SocketPeerPort(SOCKET socket) {
	sockaddr_storage address{};
	socklen_t addressLength = sizeof(address);
	if (getpeername(socket, reinterpret_cast<sockaddr*>(&address), &addressLength) != 0) {
		return 0;
	}

	if (address.ss_family == AF_INET) {
		const auto* address4 = reinterpret_cast<const sockaddr_in*>(&address);
		return ntohs(address4->sin_port);
	}

	if (address.ss_family == AF_INET6) {
		const auto* address6 = reinterpret_cast<const sockaddr_in6*>(&address);
		return ntohs(address6->sin6_port);
	}

	return 0;
}

static std::string SocketPeerIp(SOCKET socket) {
	sockaddr_storage address{};
	socklen_t addressLength = sizeof(address);
	if (getpeername(socket, reinterpret_cast<sockaddr*>(&address), &addressLength) != 0) {
		return "";
	}

	char ip[INET6_ADDRSTRLEN]{};
	if (address.ss_family == AF_INET) {
		const auto* address4 = reinterpret_cast<const sockaddr_in*>(&address);
		if (inet_ntop(AF_INET, &address4->sin_addr, ip, sizeof(ip)) == nullptr) {
			return "";
		}
	} else if (address.ss_family == AF_INET6) {
		const auto* address6 = reinterpret_cast<const sockaddr_in6*>(&address);
		if (inet_ntop(AF_INET6, &address6->sin6_addr, ip, sizeof(ip)) == nullptr) {
			return "";
		}
	} else {
		return "";
	}

	return ip;
}

static std::vector<unsigned char> HexStringToBytes(const std::string& hex) {
	std::vector<unsigned char> bytes;
	if (hex.size() % 2 != 0)
		return bytes;

	const size_t expected_len = hex.size() / 2;
	bytes.resize(expected_len);

	size_t actual_len = 0;
	int ret = sodium_hex2bin(
		bytes.data(),
		bytes.size(),      // Maximum bytes to write
		hex.c_str(),
		hex.size(),        // Length of the hex string
		nullptr,           // Ignore string (e.g., ": " to ignore colons/spaces)
		&actual_len,       // Outputs the actual number of bytes written
		nullptr            // Pointer to the end of the parsed hex string
	);

	// sodium_hex2bin returns 0 on SUCCESS, -1 on error
	if (ret != 0) {
		bytes.clear();
		return bytes;
	}

	// Shrink the vector to the actual number of bytes parsed
	bytes.resize(actual_len);
	return bytes;
}
