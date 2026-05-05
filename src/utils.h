#pragma once

static bool RecvAll(SOCKET sock, char* buffer, int length) {
	size_t total = 0;
	while (total < length) {
		size_t received = recv(sock, buffer + total, (int)(length - total), 0);
		if (received == 0) {
			return false;
		}
		total += received;
	}
	return true;
}

static bool SendAll(SOCKET sock, const char* buffer, int length) {
	size_t total = 0;
	while (total < length) {
		size_t sent = send(sock, buffer + total, (int)(length - total), 0);
		if (sent == 0) {
			return false;
		}
		total += sent;
	}
	return true;
}

