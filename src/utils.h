#pragma once

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

