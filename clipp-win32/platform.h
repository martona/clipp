#pragma once
#include <windows.h>
using PlatformWindowHandle = HWND;

static size_t utf8_to_utf16(const char* utf8, size_t n_utf8, wchar_t* utf16, size_t n_utf16) {
	return MultiByteToWideChar(CP_UTF8, 0, utf8, (int)n_utf8, utf16, (int)n_utf16);
}

static size_t utf16_to_utf8(const wchar_t* utf16, size_t n_utf16, char* utf8, size_t n_utf8) {
	return WideCharToMultiByte(CP_UTF8, 0, utf16, (int)n_utf16, utf8, (int)n_utf8, nullptr, nullptr);
}