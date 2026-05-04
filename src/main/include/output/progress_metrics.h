#pragma once

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

struct DisplayProgressMetrics {
	std::optional<int>           percent{};
	std::optional<std::uint64_t> currentBytes{};
	std::optional<std::uint64_t> totalBytes{};
	std::optional<std::uint64_t> bytesPerSecond{};
};

inline std::string progress_trim_copy(std::string_view value) {
	const auto begin = value.find_first_not_of(" \t\r\n");
	if (begin == std::string_view::npos) {
		return {};
	}
	const auto end = value.find_last_not_of(" \t\r\n");
	return std::string(value.substr(begin, end - begin + 1));
}

inline std::string progress_lower_copy(std::string_view value) {
	std::string normalized = progress_trim_copy(value);
	std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char c) {
		return static_cast<char>(std::tolower(c));
	});
	return normalized;
}

inline std::optional<std::uint64_t> normalize_progress_units(double value, std::string_view unit) {
	if (!std::isfinite(value) || value < 0.0) {
		return std::nullopt;
	}

	const std::string normalizedUnit = progress_lower_copy(unit);
	long double multiplier = 0.0L;
	if (normalizedUnit == "b" || normalizedUnit == "b/s") {
		multiplier = 1.0L;
	} else if (normalizedUnit == "kb" || normalizedUnit == "kib" || normalizedUnit == "kb/s" || normalizedUnit == "kib/s") {
		multiplier = 1024.0L;
	} else if (normalizedUnit == "mb" || normalizedUnit == "mib" || normalizedUnit == "mb/s" || normalizedUnit == "mib/s") {
		multiplier = 1024.0L * 1024.0L;
	} else if (normalizedUnit == "gb" || normalizedUnit == "gib" || normalizedUnit == "gb/s" || normalizedUnit == "gib/s") {
		multiplier = 1024.0L * 1024.0L * 1024.0L;
	} else if (normalizedUnit == "tb" || normalizedUnit == "tib" || normalizedUnit == "tb/s" || normalizedUnit == "tib/s") {
		multiplier = 1024.0L * 1024.0L * 1024.0L * 1024.0L;
	} else {
		return std::nullopt;
	}

	const long double scaled = static_cast<long double>(value) * multiplier;
	if (scaled > static_cast<long double>(std::numeric_limits<std::uint64_t>::max())) {
		return std::nullopt;
	}
	return static_cast<std::uint64_t>(std::llround(scaled));
}

inline int clamp_progress_percent(int percent) {
	return std::clamp(percent, 0, 100);
}

inline std::optional<int> resolve_progress_percent(const DisplayProgressMetrics& metrics,
	                                               std::optional<int> fallbackPercent = std::nullopt) {
	if (metrics.percent.has_value()) {
		return clamp_progress_percent(metrics.percent.value());
	}
	if (metrics.currentBytes.has_value() && metrics.totalBytes.has_value() && metrics.totalBytes.value() > 0) {
		const long double ratio = static_cast<long double>(metrics.currentBytes.value()) /
		                         static_cast<long double>(metrics.totalBytes.value());
		return clamp_progress_percent(static_cast<int>(std::llround(ratio * 100.0L)));
	}
	if (fallbackPercent.has_value()) {
		return clamp_progress_percent(fallbackPercent.value());
	}
	return std::nullopt;
}

inline DisplayProgressMetrics canonicalize_progress_metrics(DisplayProgressMetrics metrics,
	                                                       std::optional<int> fallbackPercent = std::nullopt) {
	metrics.percent = resolve_progress_percent(metrics, fallbackPercent);
	if (!metrics.currentBytes.has_value() && metrics.totalBytes.has_value() && metrics.percent.has_value()) {
		const long double current = static_cast<long double>(metrics.totalBytes.value()) *
		                          static_cast<long double>(metrics.percent.value()) / 100.0L;
		if (current >= 0.0L && current <= static_cast<long double>(std::numeric_limits<std::uint64_t>::max())) {
			metrics.currentBytes = static_cast<std::uint64_t>(std::llround(current));
		}
	}
	return metrics;
}

inline std::string humanize_progress_bytes(std::uint64_t value) {
	static constexpr const char* UNITS[] = {"B", "KiB", "MiB", "GiB", "TiB"};
	std::size_t unitIndex = 0;
	long double scaled = static_cast<long double>(value);
	while (scaled >= 1024.0L && unitIndex + 1 < (sizeof(UNITS) / sizeof(UNITS[0]))) {
		scaled /= 1024.0L;
		++unitIndex;
	}

	std::ostringstream stream;
	stream << std::fixed << std::setprecision(unitIndex == 0 ? 0 : 1) << scaled << ' ' << UNITS[unitIndex];
	return stream.str();
}

inline std::string humanize_progress_rate(std::uint64_t value) {
	return humanize_progress_bytes(value) + "/s";
}

inline std::string format_progress_summary(const DisplayProgressMetrics& rawMetrics,
	                                       std::optional<int> fallbackPercent = std::nullopt) {
	const DisplayProgressMetrics metrics = canonicalize_progress_metrics(rawMetrics, fallbackPercent);
	std::string summary;
	auto append = [&summary](const std::string& part) {
		if (part.empty()) {
			return;
		}
		if (!summary.empty()) {
			summary += "  ";
		}
		summary += part;
	};

	if (metrics.percent.has_value()) {
		append(std::to_string(metrics.percent.value()) + "%");
	}
	if (metrics.currentBytes.has_value()) {
		std::string amount = humanize_progress_bytes(metrics.currentBytes.value());
		if (metrics.totalBytes.has_value()) {
			amount += " / " + humanize_progress_bytes(metrics.totalBytes.value());
		}
		append(amount);
	}
	if (metrics.bytesPerSecond.has_value()) {
		append(humanize_progress_rate(metrics.bytesPerSecond.value()));
	}

	return summary;
}
