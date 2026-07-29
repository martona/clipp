#include <doctest/doctest.h>

#include "DefensiveLogThrottle.h"

#include <chrono>

namespace {

using namespace std::chrono_literals;

}  // namespace

TEST_CASE("defensive log throttle reports the first rejection immediately") {
	DefensiveLogThrottle throttle(60s);
	const auto now = DefensiveLogThrottle::Clock::time_point{};

	const auto report = throttle.RecordRejection(now);

	REQUIRE(report.has_value());
	CHECK(report->firstReport);
	CHECK(report->rejectedSinceLastReport == 1);
}

TEST_CASE("defensive log throttle suppresses and aggregates within its interval") {
	DefensiveLogThrottle throttle(60s);
	const auto startedAt = DefensiveLogThrottle::Clock::time_point{};

	REQUIRE(throttle.RecordRejection(startedAt).has_value());
	CHECK_FALSE(throttle.RecordRejection(startedAt + 10s).has_value());
	CHECK_FALSE(throttle.RecordRejection(startedAt + 59s).has_value());

	const auto report = throttle.RecordRejection(startedAt + 60s);
	REQUIRE(report.has_value());
	CHECK_FALSE(report->firstReport);
	CHECK(report->rejectedSinceLastReport == 3);
}

TEST_CASE("defensive log throttle reports a lone rejection after a quiet interval") {
	DefensiveLogThrottle throttle(60s);
	const auto startedAt = DefensiveLogThrottle::Clock::time_point{};

	REQUIRE(throttle.RecordRejection(startedAt).has_value());
	const auto report = throttle.RecordRejection(startedAt + 5min);

	REQUIRE(report.has_value());
	CHECK_FALSE(report->firstReport);
	CHECK(report->rejectedSinceLastReport == 1);
}
