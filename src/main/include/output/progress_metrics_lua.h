#pragma once

#include <sol/sol.hpp>

#include <optional>

#include "output/progress_metrics.h"

inline std::optional<double> lua_progress_number(const sol::table& table, const char* key) {
	const sol::object value = table[key];
	if (!value.valid() || value.is<sol::lua_nil_t>()) {
		return std::nullopt;
	}
	if (value.is<int>()) {
		return static_cast<double>(value.as<int>());
	}
	if (value.is<long long>()) {
		return static_cast<double>(value.as<long long>());
	}
	if (value.is<double>()) {
		return value.as<double>();
	}
	return std::nullopt;
}

inline std::optional<std::string> lua_progress_string(const sol::table& table, const char* key) {
	const sol::object value = table[key];
	if (!value.valid() || value.is<sol::lua_nil_t>() || !value.is<std::string>()) {
		return std::nullopt;
	}
	return value.as<std::string>();
}

inline std::optional<DisplayProgressMetrics> progress_metrics_from_lua_object(const sol::object& payload) {
	if (!payload.valid() || payload.is<sol::lua_nil_t>()) {
		return std::nullopt;
	}

	DisplayProgressMetrics metrics;
	if (payload.is<int>()) {
		metrics.percent = clamp_progress_percent(payload.as<int>());
		return metrics;
	}
	if (payload.is<double>()) {
		metrics.percent = clamp_progress_percent(static_cast<int>(std::llround(payload.as<double>())));
		return metrics;
	}
	if (payload.get_type() != sol::type::table) {
		return std::nullopt;
	}

	const sol::table table = payload.as<sol::table>();
	if (const std::optional<double> percent = lua_progress_number(table, "percent"); percent.has_value()) {
		metrics.percent = clamp_progress_percent(static_cast<int>(std::llround(percent.value())));
	}

	const std::optional<double> current = lua_progress_number(table, "current");
	const std::optional<std::string> currentUnit = lua_progress_string(table, "currentUnit");
	if (current.has_value() && currentUnit.has_value()) {
		metrics.currentBytes = normalize_progress_units(current.value(), currentUnit.value());
	}

	const std::optional<double> total = lua_progress_number(table, "total");
	const std::optional<std::string> totalUnit = lua_progress_string(table, "totalUnit");
	if (total.has_value() && totalUnit.has_value()) {
		metrics.totalBytes = normalize_progress_units(total.value(), totalUnit.value());
	}

	const std::optional<double> speed = lua_progress_number(table, "speed");
	const std::optional<std::string> speedUnit = lua_progress_string(table, "speedUnit");
	if (speed.has_value() && speedUnit.has_value()) {
		metrics.bytesPerSecond = normalize_progress_units(speed.value(), speedUnit.value());
	}

	if (!metrics.percent.has_value() && !metrics.currentBytes.has_value() && !metrics.totalBytes.has_value() && !metrics.bytesPerSecond.has_value()) {
		return std::nullopt;
	}
	return metrics;
}
