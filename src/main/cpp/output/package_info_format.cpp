#include "output/package_info_format.h"

#include "output/ansi_color.h"
#include "output/package_table_format.h"
#include "plain_display_internal.h"
#include "output/terminal/ansi.h"
#include "output/terminal/tty.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr const char* kBoxTopLeft = "\xe2\x95\xad";
constexpr const char* kBoxTopRight = "\xe2\x95\xae";
constexpr const char* kBoxBottomLeft = "\xe2\x95\xb0";
constexpr const char* kBoxBottomRight = "\xe2\x95\xaf";
constexpr const char* kBoxHorizontal = "\xe2\x94\x80";
constexpr const char* kBoxVertical = "\xe2\x94\x82";
constexpr const char* kTreeBranch = "\xe2\x94\x9c\xe2\x94\x80";
constexpr const char* kTreeLast = "\xe2\x94\x94\xe2\x94\x80";

struct Style {
	bool color{false};
	bool hyperlinks{false};
};

std::string trim_copy(std::string value) {
	while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
		value.erase(value.begin());
	}
	while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
		value.pop_back();
	}
	return value;
}

std::string to_lower_ascii(std::string value) {
	for (char& character : value) {
		character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
	}
	return value;
}

bool iequals_trimmed(const std::string& left, const std::string& right) {
	return to_lower_ascii(trim_copy(left)) == to_lower_ascii(trim_copy(right));
}

std::string wrap(const std::string& text, const std::string& spec, const Style& style) {
	if (!style.color || spec.empty()) {
		return text;
	}
	return ansi_wrap(text, spec);
}

std::string strip_url_scheme(std::string url) {
	if (url.rfind("https://", 0) == 0) {
		return url.substr(8);
	}
	if (url.rfind("http://", 0) == 0) {
		return url.substr(7);
	}
	return url;
}

std::string shorten_url_display(const std::string& url, const size_t maxLength) {
	if (maxLength == 0) {
		return {};
	}
	const std::string hostPath = strip_url_scheme(url);
	if (plain_display_internal::visual_length(hostPath) <= maxLength) {
		return hostPath;
	}
	return plain_display_internal::truncate_middle(hostPath, maxLength);
}

std::string format_link_display(const std::string& url, const size_t maxLength, const Style& style) {
	if (url.empty()) {
		return {};
	}
	constexpr const char* kLinkArrow = " \xe2\x86\x97";
	const size_t labelBudget = maxLength > plain_display_internal::visual_length(kLinkArrow)
	                               ? maxLength - plain_display_internal::visual_length(kLinkArrow)
	                               : maxLength;
	const std::string shortened = shorten_url_display(url, labelBudget);
	const std::string linked = reqpack::terminal::osc8_hyperlink(url, shortened, style.hyperlinks);
	std::string rendered = linked;
	if (style.color) {
		rendered = wrap(linked, "dim underline", style);
	}
	rendered += style.color ? wrap(kLinkArrow, "bright_black", style) : kLinkArrow;
	return rendered;
}

std::optional<std::chrono::system_clock::time_point> parse_iso8601(const std::string& raw) {
	const std::string value = trim_copy(raw);
	if (value.size() < 10) {
		return std::nullopt;
	}

	std::tm parts {};
	parts.tm_year = std::stoi(value.substr(0, 4)) - 1900;
	parts.tm_mon = std::stoi(value.substr(5, 2)) - 1;
	parts.tm_mday = std::stoi(value.substr(8, 2));

	if (value.size() >= 19 && value[10] == 'T') {
		parts.tm_hour = std::stoi(value.substr(11, 2));
		parts.tm_min = std::stoi(value.substr(14, 2));
		parts.tm_sec = std::stoi(value.substr(17, 2));
	}

#if defined(_WIN32)
	const std::time_t seconds = _mkgmtime(&parts);
#else
	const std::time_t seconds = timegm(&parts);
#endif
	if (seconds == static_cast<std::time_t>(-1)) {
		return std::nullopt;
	}
	return std::chrono::system_clock::from_time_t(seconds);
}

std::string month_name_short(int month) {
	static const char* names[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
	                              "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
	if (month < 1 || month > 12) {
		return "?";
	}
	return names[month - 1];
}

std::string relative_age_phrase(const std::chrono::system_clock::time_point& when) {
	using Days = std::chrono::duration<int, std::ratio<86400>>;
	const auto now = std::chrono::system_clock::now();
	const int days = static_cast<int>(std::chrono::duration_cast<Days>(now - when).count());
	if (days < 0) {
		return "in the future";
	}
	if (days == 0) {
		return "today";
	}
	if (days == 1) {
		return "1 day ago";
	}
	if (days < 14) {
		return std::to_string(days) + " days ago";
	}
	if (days < 45) {
		const int weeks = (days + 6) / 7;
		return std::to_string(weeks) + (weeks == 1 ? " week ago" : " weeks ago");
	}
	if (days < 330) {
		const int months = std::max(1, days / 30);
		return std::to_string(months) + (months == 1 ? " month ago" : " months ago");
	}
	const int years = std::max(1, days / 365);
	return std::to_string(years) + (years == 1 ? " year ago" : " years ago");
}

std::string format_published_date(const std::string& raw) {
	const std::optional<std::chrono::system_clock::time_point> parsed = parse_iso8601(raw);
	if (!parsed.has_value()) {
		return raw;
	}
	const std::time_t seconds = std::chrono::system_clock::to_time_t(*parsed);
	std::tm utc {};
#if defined(_WIN32)
	gmtime_s(&utc, &seconds);
#else
	gmtime_r(&seconds, &utc);
#endif
	std::ostringstream formatted;
	formatted << utc.tm_mday << ' ' << month_name_short(utc.tm_mon + 1) << ' ' << (utc.tm_year + 1900);
	const std::string relative = relative_age_phrase(*parsed);
	if (!relative.empty()) {
		formatted << " (" << relative << ')';
	}
	return formatted.str();
}

std::string system_label(const PackageInfo& info) {
	std::ostringstream stream;
	if (!info.system.empty()) {
		stream << info.system;
	}
	const std::string typeLabel = !info.packageType.empty() ? info.packageType : package_table_type_badge(info.packageType);
	if (!typeLabel.empty()) {
		if (!info.system.empty()) {
			stream << " (";
		}
		stream << typeLabel;
		if (!info.system.empty()) {
			stream << ')';
		}
	}
	return stream.str();
}

std::string author_line(const PackageInfo& info) {
	std::string person;
	if (!info.author.empty()) {
		person = info.author;
	} else if (!info.maintainer.empty()) {
		person = info.maintainer;
	}
	if (person.empty()) {
		return {};
	}
	if (!info.email.empty() && person.find('<') == std::string::npos) {
		person += " <" + info.email + '>';
	}
	return person;
}

struct StatusBadge {
	std::string text;
	std::string colorSpec;
};

StatusBadge resolve_status_badge(const PackageInfo& info) {
	const bool installed = info.installed == "true" || iequals_trimmed(info.status, "installed");
	const bool hasUpdate = installed && !info.latestVersion.empty() && !iequals_trimmed(info.latestVersion, info.version);

	if (hasUpdate) {
		return {"\xe2\x86\x91 Update available", "yellow"};
	}
	if (installed) {
		return {"\xe2\x97\x8f Installed", "green"};
	}
	if (iequals_trimmed(info.status, "available") || info.installed == "false") {
		return {"\xe2\x97\x8b Not installed", "bright_black"};
	}
	if (!info.status.empty()) {
		return {info.status, "bright_black"};
	}
	return {"\xe2\x97\x8b Not installed", "bright_black"};
}

std::string repeat_unit(const char* unit, size_t count) {
	std::string result;
	for (size_t index = 0; index < count; ++index) {
		result += unit;
	}
	return result;
}

std::string fit_visual_width(const std::string& content, const size_t width) {
	if (width == 0) {
		return {};
	}
	const size_t length = plain_display_internal::visual_length(content);
	if (length < width) {
		return content + std::string(width - length, ' ');
	}
	if (length > width) {
		return plain_display_internal::truncate_end(content, width);
	}
	return content;
}

std::string format_box_line(const std::string& inner, const size_t boxInnerWidth) {
	return std::string(kBoxVertical) + fit_visual_width(inner, boxInnerWidth) + kBoxVertical + '\n';
}

std::string compose_box_title_row(const std::string& left,
                                  const std::string& right,
                                  const size_t rowContentWidth) {
	const size_t rightWidth = plain_display_internal::visual_length(right);
	const size_t minGap = 1;
	const size_t maxLeftWidth = rowContentWidth > rightWidth + minGap ? rowContentWidth - rightWidth - minGap : 1;

	std::string fittedLeft = left;
	if (plain_display_internal::visual_length(fittedLeft) > maxLeftWidth) {
		fittedLeft = plain_display_internal::truncate_end(fittedLeft, maxLeftWidth);
	}

	const size_t leftWidth = plain_display_internal::visual_length(fittedLeft);
	size_t gap = rowContentWidth - leftWidth - rightWidth;
	if (gap < minGap) {
		gap = minGap;
	}

	std::string row = fittedLeft + std::string(gap, ' ') + right;
	while (plain_display_internal::visual_length(row) > rowContentWidth
	       && plain_display_internal::visual_length(fittedLeft) > 0) {
		fittedLeft = plain_display_internal::truncate_end(fittedLeft, plain_display_internal::visual_length(fittedLeft) - 1);
		const size_t leftWidth = plain_display_internal::visual_length(fittedLeft);
		gap = rowContentWidth > leftWidth + rightWidth ? rowContentWidth - leftWidth - rightWidth : 0;
		row = fittedLeft + std::string(gap, ' ') + right;
	}
	return fit_visual_width(row, rowContentWidth);
}

std::string format_tags_metadata_value(const std::vector<std::string>& tags, const Style& style) {
	std::ostringstream stream;
	for (size_t index = 0; index < tags.size(); ++index) {
		if (index > 0) {
			stream << "  ";
		}
		const std::string token = '#' + tags[index];
		stream << (style.color ? wrap(token, "cyan", style) : token);
	}
	return stream.str();
}

std::string join_values(const std::vector<std::string>& values) {
	std::ostringstream stream;
	for (size_t index = 0; index < values.size(); ++index) {
		if (index > 0) {
			stream << ", ";
		}
		stream << values[index];
	}
	return stream.str();
}

struct SectionRow {
	std::string label;
	std::string value;
	bool isUrl{false};
};

void append_row(std::vector<SectionRow>& rows, const std::string& label, const std::string& value, bool isUrl = false) {
	if (!value.empty()) {
		rows.push_back(SectionRow{.label = label, .value = value, .isUrl = isUrl});
	}
}

std::string render_section(const std::string& title,
                           const std::vector<SectionRow>& rows,
                           const Style& style,
                           size_t labelWidth,
                           size_t urlMaxWidth) {
	if (rows.empty()) {
		return {};
	}

	std::ostringstream stream;
	stream << '\n';
	stream << "  " << wrap(title, "bold", style) << '\n';
	for (size_t index = 0; index < rows.size(); ++index) {
		const bool last = index + 1 == rows.size();
		const std::string prefix = last ? kTreeLast : kTreeBranch;
		const std::string label = wrap(rows[index].label, "bright_black", style);
		const std::string paddedLabel = plain_display_internal::pad_right_visual(label, labelWidth);
		std::string value = rows[index].value;
		if (rows[index].isUrl) {
			value = format_link_display(value, urlMaxWidth, style);
		} else if (style.color) {
			value = wrap(value, "white", style);
		}
		stream << "  " << prefix << ' ' << paddedLabel << "  " << value << '\n';
	}
	return stream.str();
}

std::string render_description_block(const std::string& description, const Style& style, size_t terminalWidth) {
	if (description.empty()) {
		return {};
	}
	const size_t width = terminalWidth > 4 ? terminalWidth - 2 : 72;
	std::ostringstream stream;
	stream << '\n';
	// Simple wrap: split on spaces
	std::istringstream words(description);
	std::string word;
	std::string line;
	while (words >> word) {
		if (line.empty()) {
			line = word;
			continue;
		}
		if (line.size() + 1 + word.size() <= width) {
			line += ' ';
			line += word;
			continue;
		}
		stream << wrap(line, "dim white", style) << '\n';
		line = word;
	}
	if (!line.empty()) {
		stream << wrap(line, "dim white", style) << '\n';
	}
	return stream.str();
}

}  // namespace

std::string format_package_info(const PackageInfo& info) {
	const Style style {
	    .color = reqpack::terminal::use_animated_terminal_ui(),
	    .hyperlinks = reqpack::terminal::use_animated_terminal_ui(),
	};

	const size_t terminalWidth = std::max<size_t>(plain_display_internal::terminal_width(), 62);
	const size_t layoutWidth = std::min<size_t>(terminalWidth, 80);
	const size_t boxInnerWidth = layoutWidth >= 2 ? layoutWidth - 2 : 78;
	const size_t titleContentWidth = boxInnerWidth >= 2 ? boxInnerWidth - 2 : 76;

	const std::string displayName = package_table_display_name(info.name.empty() ? info.packageId : info.name);
	const std::string version = info.version;
	const StatusBadge badge = resolve_status_badge(info);

	const std::string summary = trim_copy(package_table_clean_description(info));
	const std::string description = trim_copy(info.description);
	const bool showLongDescription = !description.empty() && !iequals_trimmed(description, summary);

	std::ostringstream titleLeft;
	titleLeft << "\xf0\x9f\x93\xa6 " << wrap(displayName, "bold cyan", style);
	if (!version.empty()) {
		titleLeft << ' ' << wrap(version, "yellow", style);
	}

	const std::string badgeText = wrap(badge.text, badge.colorSpec, style);
	const std::string headerRendered = compose_box_title_row(titleLeft.str(), badgeText, titleContentWidth);

	std::ostringstream output;
	output << '\n';
	output << kBoxTopLeft << repeat_unit(kBoxHorizontal, boxInnerWidth) << kBoxTopRight << '\n';
	output << format_box_line(' ' + headerRendered, boxInnerWidth);
	if (!summary.empty()) {
		output << format_box_line(' ' + wrap(summary, "dim white", style), boxInnerWidth);
	}
	output << kBoxBottomLeft << repeat_unit(kBoxHorizontal, boxInnerWidth) << kBoxBottomRight << "\n\n";

	const size_t sectionLabelWidth = 11;
	const size_t treeValueOffset = 2 + 3 + 1 + sectionLabelWidth + 2;
	const size_t urlMaxWidth = layoutWidth > treeValueOffset ? layoutWidth - treeValueOffset : 48;

	std::vector<SectionRow> metadata;
	append_row(metadata, "System", system_label(info));
	const std::string target = package_table_platform_label(info);
	append_row(metadata, "Target", target);
	append_row(metadata, "License", info.license);
	append_row(metadata, "Author", author_line(info));
	if (!info.maintainer.empty() && !iequals_trimmed(info.maintainer, info.author)) {
		append_row(metadata, "Maintainer", info.maintainer);
	}
	if (!info.publishedAt.empty()) {
		append_row(metadata, "Published", format_published_date(info.publishedAt));
	}
	if (!info.updatedAt.empty() && !iequals_trimmed(info.updatedAt, info.publishedAt)) {
		append_row(metadata, "Updated", format_published_date(info.updatedAt));
	}
	if (!info.channel.empty()) {
		append_row(metadata, "Channel", info.channel);
	}
	if (!info.section.empty()) {
		append_row(metadata, "Section", info.section);
	}
	if (!info.size.empty()) {
		append_row(metadata, "Size", info.size);
	}
	if (!info.installedSize.empty()) {
		append_row(metadata, "Installed size", info.installedSize);
	}
	if (!info.latestVersion.empty() && !iequals_trimmed(info.latestVersion, info.version)) {
		append_row(metadata, "Latest", info.latestVersion);
	}
	if (!info.binaries.empty() && (info.installed == "true" || iequals_trimmed(info.status, "installed"))) {
		append_row(metadata, "Binaries", join_values(info.binaries));
	}
	if (!info.tags.empty()) {
		append_row(metadata, "Tags", format_tags_metadata_value(info.tags, style));
	}

	output << render_section("Metadata", metadata, style, sectionLabelWidth, urlMaxWidth);

	std::vector<SectionRow> links;
	const std::string source = !info.sourceUrl.empty() ? info.sourceUrl : info.repository;
	append_row(links, "Source", source, true);
	if (!info.homepage.empty() && !iequals_trimmed(info.homepage, source)) {
		append_row(links, "Artifact", info.homepage, true);
	}
	if (!info.documentation.empty() && !iequals_trimmed(info.documentation, source)
	    && !iequals_trimmed(info.documentation, info.homepage)) {
		append_row(links, "Docs", info.documentation, true);
	}
	if (!info.repository.empty() && !iequals_trimmed(info.repository, source)) {
		append_row(links, "Registry", info.repository, true);
	}
	output << render_section("Links", links, style, sectionLabelWidth, urlMaxWidth);

	std::vector<SectionRow> relations;
	append_row(relations, "Depends", join_values(info.dependencies));
	append_row(relations, "Optional", join_values(info.optionalDependencies));
	append_row(relations, "Provides", join_values(info.provides));
	append_row(relations, "Conflicts", join_values(info.conflicts));
	append_row(relations, "Replaces", join_values(info.replaces));
	output << render_section("Relations", relations, style, sectionLabelWidth, urlMaxWidth);

	if (!info.extraFields.empty()) {
		std::vector<SectionRow> extra;
		for (const auto& [key, value] : info.extraFields) {
			append_row(extra, key, value);
		}
		output << render_section("Details", extra, style, sectionLabelWidth, urlMaxWidth);
	}

	if (showLongDescription) {
		output << render_description_block(description, style, terminalWidth);
	}

	output << '\n';
	return output.str();
}
