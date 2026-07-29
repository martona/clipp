#pragma once

#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>

// Event-driven rate limiter for defensive-mechanism logging. Callers provide
// their own synchronization; PeerManager uses the locks already protecting each
// admission decision. No timer or per-source state is retained.
class DefensiveLogThrottle {
public:
	using Clock = std::chrono::steady_clock;

	struct Report {
		std::uint64_t rejectedSinceLastReport;
		bool firstReport;
	};

	explicit DefensiveLogThrottle(
		Clock::duration reportInterval = std::chrono::minutes(1))
		: reportInterval_(reportInterval) {}

	std::optional<Report> RecordRejection(Clock::time_point now = Clock::now()) {
		if (rejectedSinceLastReport_ < (std::numeric_limits<std::uint64_t>::max)()) {
			++rejectedSinceLastReport_;
		}

		if (hasReported_ && now - lastReportAt_ < reportInterval_) {
			return std::nullopt;
		}

		const Report report{ rejectedSinceLastReport_, !hasReported_ };
		rejectedSinceLastReport_ = 0;
		lastReportAt_ = now;
		hasReported_ = true;
		return report;
	}

private:
	Clock::duration reportInterval_;
	Clock::time_point lastReportAt_{};
	std::uint64_t rejectedSinceLastReport_{ 0 };
	bool hasReported_{ false };
};
