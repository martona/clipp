#pragma once

static bool RecvAll(SOCKET sock, char* buffer, int length) {
	int total = 0;
	while (total < length) {
		int received = recv(sock, buffer + total, (int)(length - total), 0);
		if (received <= 0) {
			return false;
		}
		total += received;
	}
	return true;
}

static bool SendAll(SOCKET sock, const char* buffer, int length) {
	int total = 0;
	while (total < length) {
		int sent = send(sock, buffer + total, (int)(length - total), 0);
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
