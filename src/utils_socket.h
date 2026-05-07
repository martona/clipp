#pragma once

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
	}
	else if (address.ss_family == AF_INET6) {
		const auto* address6 = reinterpret_cast<const sockaddr_in6*>(&address);
		if (inet_ntop(AF_INET6, &address6->sin6_addr, ip, sizeof(ip)) == nullptr) {
			return "";
		}
	}
	else {
		return "";
	}

	return ip;
}

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

