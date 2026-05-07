#pragma once
#include <chrono>

struct ScopedTimer {
	const wchar_t* operationName;
	std::chrono::steady_clock::time_point start;

	ScopedTimer(const wchar_t* name) : operationName(name) {
		start = std::chrono::steady_clock::now();
	}

	~ScopedTimer() {
		auto end = std::chrono::steady_clock::now();
		auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
		g_logger.log("ScopedTimer", Logger::Level::Debug, L"%ls took %lld ms", operationName, duration.count());
	}
};
