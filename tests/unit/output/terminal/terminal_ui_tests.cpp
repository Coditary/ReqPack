#include <catch2/catch.hpp>

#include "output/terminal/block_progress_bar.h"
#include "output/terminal/live_progress_controller.h"
#include "output/terminal/progress_phase.h"
#include "output/terminal/render_throttle.h"
#include "output/terminal/speed_ema.h"

#include <algorithm>
#include <chrono>
#include <sstream>

TEST_CASE("block progress bar uses full-block unicode fill", "[unit][terminal]") {
	CHECK(reqpack::terminal::render_fractional_block_bar(0.0, 10).find("░") != std::string::npos);
	CHECK(reqpack::terminal::render_fractional_block_bar(1.0, 10).find("█") != std::string::npos);
	const std::string half = reqpack::terminal::render_fractional_block_bar(0.5, 20);
	CHECK(half.find("█") != std::string::npos);
	CHECK(half.find("░") != std::string::npos);
	CHECK(half.size() == 60);
}

TEST_CASE("progress phase hides transfer metrics after extract", "[unit][terminal]") {
	CHECK(reqpack::terminal::classify_progress_step("Downloading archive") == reqpack::terminal::ProgressPhase::TRANSFER);
	CHECK(reqpack::terminal::classify_progress_step("extract release archive") ==
	      reqpack::terminal::ProgressPhase::POST_TRANSFER);

	DisplayProgressMetrics metrics{
	    .percent = 82,
	    .currentBytes = 116391936,
	    .totalBytes = 116391936,
	    .bytesPerSecond = 108213043,
	};
	const auto trimmed =
	    reqpack::terminal::metrics_for_display_phase(metrics, reqpack::terminal::ProgressPhase::POST_TRANSFER);
	CHECK_FALSE(trimmed.currentBytes.has_value());
	CHECK_FALSE(trimmed.bytesPerSecond.has_value());
	CHECK(trimmed.percent.value() == 82);
}

TEST_CASE("speed ema smooths noisy samples", "[unit][terminal]") {
	reqpack::terminal::SpeedEma ema;
	ema.observe(100);
	ema.observe(1000);
	REQUIRE(ema.value().has_value());
	CHECK(ema.value().value() > 100);
	CHECK(ema.value().value() < 1000);
}

TEST_CASE("complete_item prints outcome line only once", "[unit][terminal]") {
	std::ostringstream stream;
	reqpack::terminal::LiveProgressController controller(
	    stream,
	    [](const std::string& itemId, const DisplayItemStatus&, const std::string_view, const reqpack::terminal::LiveProgressLineStyle&) {
		    return itemId;
	    });
	controller.begin_session("Installing...");
	controller.begin_item("dnf:gimp", "gimp");
	const std::string marker = "  └─ ✔dnf:gimp done";
	controller.complete_item("dnf:gimp", true, marker);
	controller.complete_item("dnf:gimp", true, marker);
	const std::string rendered = stream.str();
	CHECK(std::count(rendered.begin(), rendered.end(), '\n') >= 1);
	const auto first = rendered.find(marker);
	REQUIRE(first != std::string::npos);
	CHECK(rendered.find(marker, first + marker.size()) == std::string::npos);
}

TEST_CASE("render throttle limits update rate", "[unit][terminal]") {
	using Clock = std::chrono::steady_clock;
	Clock::time_point last{};
	const auto t0 = Clock::now();
	CHECK(reqpack::terminal::should_render(t0, last));
	CHECK_FALSE(reqpack::terminal::should_render(t0 + std::chrono::milliseconds(5), last));
	CHECK(reqpack::terminal::should_render(t0 + std::chrono::milliseconds(40), last));
	CHECK(reqpack::terminal::should_render(t0 + std::chrono::milliseconds(50), last, reqpack::terminal::kDefaultRenderInterval, true));
}
