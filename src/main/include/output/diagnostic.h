#pragma once

#include <spdlog/spdlog.h>

#include <string>
#include <utility>
#include <vector>

using DiagnosticField = std::pair<std::string, std::string>;
using DiagnosticFields = std::vector<DiagnosticField>;

struct DiagnosticMessage {
	spdlog::level::level_enum severity{spdlog::level::info};
	std::string               category{};
	std::string               summary{};
	std::string               cause{};
	std::string               recommendation{};
	std::string               details{};
	std::string               source{};
	std::string               scope{};
	DiagnosticFields          context{};
};

inline DiagnosticMessage make_diagnostic(
	spdlog::level::level_enum severity,
	std::string category,
	std::string summary,
	std::string cause = {},
	std::string recommendation = {},
	std::string details = {},
	std::string source = {},
	std::string scope = {},
	DiagnosticFields context = {}
) {
	return DiagnosticMessage{
		.severity = severity,
		.category = std::move(category),
		.summary = std::move(summary),
		.cause = std::move(cause),
		.recommendation = std::move(recommendation),
		.details = std::move(details),
		.source = std::move(source),
		.scope = std::move(scope),
		.context = std::move(context),
	};
}

inline DiagnosticMessage make_error_diagnostic(
	std::string category,
	std::string summary,
	std::string cause = {},
	std::string recommendation = {},
	std::string details = {},
	std::string source = {},
	std::string scope = {},
	DiagnosticFields context = {}
) {
	return make_diagnostic(
		spdlog::level::err,
		std::move(category),
		std::move(summary),
		std::move(cause),
		std::move(recommendation),
		std::move(details),
		std::move(source),
		std::move(scope),
		std::move(context)
	);
}

inline DiagnosticMessage make_warning_diagnostic(
	std::string category,
	std::string summary,
	std::string cause = {},
	std::string recommendation = {},
	std::string details = {},
	std::string source = {},
	std::string scope = {},
	DiagnosticFields context = {}
) {
	return make_diagnostic(
		spdlog::level::warn,
		std::move(category),
		std::move(summary),
		std::move(cause),
		std::move(recommendation),
		std::move(details),
		std::move(source),
		std::move(scope),
		std::move(context)
	);
}
