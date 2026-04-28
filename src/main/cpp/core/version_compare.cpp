#include "core/version_compare.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <regex>
#include <sstream>
#include <vector>

namespace {

struct SemverValue {
    std::vector<std::string> core;
    std::vector<std::string> prerelease;
};

struct Pep440Value {
    long epoch{0};
    std::vector<std::string> release;
    int stageRank{4};
    long stageNumber{0};
    bool hasDev{false};
    long devNumber{0};
    std::vector<std::string> local;
};

struct RpmEvrValue {
    long epoch{0};
    std::string version;
    std::string release;
};

std::string lower_copy(const std::string& value) {
    std::string normalized = value;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return normalized;
}

bool is_numeric_token(const std::string& value) {
    return !value.empty() && std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return std::isdigit(c) != 0;
    });
}

std::string trim_leading_zeroes(const std::string& value) {
    const std::size_t position = value.find_first_not_of('0');
    return position == std::string::npos ? "0" : value.substr(position);
}

int compare_numeric_strings(const std::string& left, const std::string& right) {
    const std::string normalizedLeft = trim_leading_zeroes(left);
    const std::string normalizedRight = trim_leading_zeroes(right);
    if (normalizedLeft.size() != normalizedRight.size()) {
        return normalizedLeft.size() < normalizedRight.size() ? -1 : 1;
    }
    if (normalizedLeft == normalizedRight) {
        return 0;
    }
    return normalizedLeft < normalizedRight ? -1 : 1;
}

int compare_tokens(const std::string& left, const std::string& right);

std::vector<std::string> split_on_char(const std::string& value, const char separator) {
    std::vector<std::string> parts;
    std::stringstream stream(value);
    std::string part;
    while (std::getline(stream, part, separator)) {
        parts.push_back(part);
    }
    if (parts.empty()) {
        parts.push_back(value);
    }
    return parts;
}

SemverValue parse_semver(const std::string& raw) {
    std::string value = raw;
    if (!value.empty() && (value.front() == 'v' || value.front() == 'V') && value.size() > 1 && std::isdigit(static_cast<unsigned char>(value[1]))) {
        value.erase(value.begin());
    }

    const std::size_t buildSeparator = value.find('+');
    if (buildSeparator != std::string::npos) {
        value = value.substr(0, buildSeparator);
    }

    const std::size_t prereleaseSeparator = value.find('-');
    SemverValue parsed;
    parsed.core = split_on_char(value.substr(0, prereleaseSeparator), '.');
    if (prereleaseSeparator != std::string::npos) {
        parsed.prerelease = split_on_char(value.substr(prereleaseSeparator + 1), '.');
    }
    return parsed;
}

int compare_semver_identifiers(const std::string& left, const std::string& right) {
    const bool leftNumeric = is_numeric_token(left);
    const bool rightNumeric = is_numeric_token(right);
    if (leftNumeric && rightNumeric) {
        return compare_numeric_strings(left, right);
    }
    if (leftNumeric != rightNumeric) {
        return leftNumeric ? -1 : 1;
    }
    if (left == right) {
        return 0;
    }
    return left < right ? -1 : 1;
}

int compare_semver(const std::string& left, const std::string& right) {
    const SemverValue parsedLeft = parse_semver(left);
    const SemverValue parsedRight = parse_semver(right);
    const std::size_t maxCoreSize = std::max<std::size_t>(3, std::max(parsedLeft.core.size(), parsedRight.core.size()));
    for (std::size_t index = 0; index < maxCoreSize; ++index) {
        const std::string leftPart = index < parsedLeft.core.size() ? parsedLeft.core[index] : std::string{"0"};
        const std::string rightPart = index < parsedRight.core.size() ? parsedRight.core[index] : std::string{"0"};
        const int comparison = compare_numeric_strings(leftPart, rightPart);
        if (comparison != 0) {
            return comparison;
        }
    }

    if (parsedLeft.prerelease.empty() && parsedRight.prerelease.empty()) {
        return 0;
    }
    if (parsedLeft.prerelease.empty()) {
        return 1;
    }
    if (parsedRight.prerelease.empty()) {
        return -1;
    }

    const std::size_t maxPrereleaseSize = std::max(parsedLeft.prerelease.size(), parsedRight.prerelease.size());
    for (std::size_t index = 0; index < maxPrereleaseSize; ++index) {
        if (index >= parsedLeft.prerelease.size()) {
            return -1;
        }
        if (index >= parsedRight.prerelease.size()) {
            return 1;
        }
        const int comparison = compare_semver_identifiers(parsedLeft.prerelease[index], parsedRight.prerelease[index]);
        if (comparison != 0) {
            return comparison;
        }
    }

    return 0;
}

std::string normalize_pep440(std::string value) {
    value = lower_copy(value);
    std::replace(value.begin(), value.end(), '-', '.');
    std::replace(value.begin(), value.end(), '_', '.');
    if (!value.empty() && value.front() == 'v' && value.size() > 1 && std::isdigit(static_cast<unsigned char>(value[1]))) {
        value.erase(value.begin());
    }
    return value;
}

long parse_optional_number(const std::string& value) {
    if (value.empty()) {
        return 0;
    }
    try {
        return std::stol(value);
    } catch (...) {
        return 0;
    }
}

std::vector<std::string> split_local_tokens(const std::string& value) {
    std::vector<std::string> tokens;
    if (value.empty()) {
        return tokens;
    }
    std::regex separator(R"([._-]+)");
    std::sregex_token_iterator it(value.begin(), value.end(), separator, -1);
    std::sregex_token_iterator end;
    for (; it != end; ++it) {
        if (!it->str().empty()) {
            tokens.push_back(it->str());
        }
    }
    return tokens;
}

Pep440Value parse_pep440(const std::string& raw) {
    std::string value = normalize_pep440(raw);
    Pep440Value parsed;

    const std::size_t epochSeparator = value.find('!');
    if (epochSeparator != std::string::npos) {
        parsed.epoch = parse_optional_number(value.substr(0, epochSeparator));
        value = value.substr(epochSeparator + 1);
    }

    const std::size_t localSeparator = value.find('+');
    if (localSeparator != std::string::npos) {
        parsed.local = split_local_tokens(value.substr(localSeparator + 1));
        value = value.substr(0, localSeparator);
    }

    std::smatch match;
    const std::regex versionRegex(R"(^([0-9]+(?:\.[0-9]+)*)(?:(a|alpha|b|beta|rc|c|pre|preview)([0-9]*))?(?:(?:\.)?(post|rev|r)([0-9]*))?(?:(?:\.)?dev([0-9]*))?$)");
    if (!std::regex_match(value, match, versionRegex)) {
        parsed.release = split_on_char(value, '.');
        return parsed;
    }

    parsed.release = split_on_char(match[1].str(), '.');
    const std::string prereleaseTag = match[2].str();
    const std::string prereleaseNumber = match[3].str();
    const std::string postTag = match[4].str();
    const std::string postNumber = match[5].str();
    const std::string devNumber = match[6].str();

    if (!prereleaseTag.empty()) {
        if (prereleaseTag == "a" || prereleaseTag == "alpha") {
            parsed.stageRank = 1;
        } else if (prereleaseTag == "b" || prereleaseTag == "beta") {
            parsed.stageRank = 2;
        } else {
            parsed.stageRank = 3;
        }
        parsed.stageNumber = parse_optional_number(prereleaseNumber);
    } else if (!postTag.empty()) {
        parsed.stageRank = 5;
        parsed.stageNumber = parse_optional_number(postNumber);
    } else if (!devNumber.empty()) {
        parsed.stageRank = 0;
    }

    if (!devNumber.empty()) {
        parsed.hasDev = true;
        parsed.devNumber = parse_optional_number(devNumber);
    }

    return parsed;
}

int compare_pep440(const std::string& left, const std::string& right) {
    const Pep440Value parsedLeft = parse_pep440(left);
    const Pep440Value parsedRight = parse_pep440(right);
    if (parsedLeft.epoch != parsedRight.epoch) {
        return parsedLeft.epoch < parsedRight.epoch ? -1 : 1;
    }

    const std::size_t maxReleaseSize = std::max(parsedLeft.release.size(), parsedRight.release.size());
    for (std::size_t index = 0; index < maxReleaseSize; ++index) {
        const std::string leftPart = index < parsedLeft.release.size() ? parsedLeft.release[index] : std::string{"0"};
        const std::string rightPart = index < parsedRight.release.size() ? parsedRight.release[index] : std::string{"0"};
        const int comparison = compare_numeric_strings(leftPart, rightPart);
        if (comparison != 0) {
            return comparison;
        }
    }

    if (parsedLeft.stageRank != parsedRight.stageRank) {
        return parsedLeft.stageRank < parsedRight.stageRank ? -1 : 1;
    }
    if (parsedLeft.stageNumber != parsedRight.stageNumber) {
        return parsedLeft.stageNumber < parsedRight.stageNumber ? -1 : 1;
    }
    if (parsedLeft.hasDev != parsedRight.hasDev) {
        return parsedLeft.hasDev ? -1 : 1;
    }
    if (parsedLeft.devNumber != parsedRight.devNumber) {
        return parsedLeft.devNumber < parsedRight.devNumber ? -1 : 1;
    }

    const std::size_t maxLocalSize = std::max(parsedLeft.local.size(), parsedRight.local.size());
    for (std::size_t index = 0; index < maxLocalSize; ++index) {
        if (index >= parsedLeft.local.size()) {
            return -1;
        }
        if (index >= parsedRight.local.size()) {
            return 1;
        }
        const int comparison = compare_tokens(parsedLeft.local[index], parsedRight.local[index]);
        if (comparison != 0) {
            return comparison;
        }
    }

    return 0;
}

RpmEvrValue parse_rpm_evr(const std::string& raw) {
    std::string value = raw;
    RpmEvrValue parsed;
    const std::size_t epochSeparator = value.find(':');
    if (epochSeparator != std::string::npos) {
        parsed.epoch = parse_optional_number(value.substr(0, epochSeparator));
        value = value.substr(epochSeparator + 1);
    }

    const std::size_t releaseSeparator = value.rfind('-');
    parsed.version = releaseSeparator == std::string::npos ? value : value.substr(0, releaseSeparator);
    parsed.release = releaseSeparator == std::string::npos ? std::string{} : value.substr(releaseSeparator + 1);
    return parsed;
}

int rpmvercmp(const std::string& left, const std::string& right) {
    std::size_t leftIndex = 0;
    std::size_t rightIndex = 0;
    while (leftIndex < left.size() || rightIndex < right.size()) {
        while (leftIndex < left.size() && !std::isalnum(static_cast<unsigned char>(left[leftIndex])) && left[leftIndex] != '~') {
            ++leftIndex;
        }
        while (rightIndex < right.size() && !std::isalnum(static_cast<unsigned char>(right[rightIndex])) && right[rightIndex] != '~') {
            ++rightIndex;
        }

        const bool leftTilde = leftIndex < left.size() && left[leftIndex] == '~';
        const bool rightTilde = rightIndex < right.size() && right[rightIndex] == '~';
        if (leftTilde || rightTilde) {
            if (!leftTilde) {
                return 1;
            }
            if (!rightTilde) {
                return -1;
            }
            ++leftIndex;
            ++rightIndex;
            continue;
        }

        if (leftIndex >= left.size() && rightIndex >= right.size()) {
            return 0;
        }
        if (leftIndex >= left.size()) {
            return -1;
        }
        if (rightIndex >= right.size()) {
            return 1;
        }

        const bool leftNumeric = std::isdigit(static_cast<unsigned char>(left[leftIndex])) != 0;
        const bool rightNumeric = std::isdigit(static_cast<unsigned char>(right[rightIndex])) != 0;
        if (leftNumeric != rightNumeric) {
            return leftNumeric ? 1 : -1;
        }

        const std::size_t leftSegmentStart = leftIndex;
        const std::size_t rightSegmentStart = rightIndex;
        while (leftIndex < left.size() && (leftNumeric ? std::isdigit(static_cast<unsigned char>(left[leftIndex])) != 0 : std::isalpha(static_cast<unsigned char>(left[leftIndex])) != 0)) {
            ++leftIndex;
        }
        while (rightIndex < right.size() && (rightNumeric ? std::isdigit(static_cast<unsigned char>(right[rightIndex])) != 0 : std::isalpha(static_cast<unsigned char>(right[rightIndex])) != 0)) {
            ++rightIndex;
        }

        const std::string leftSegment = left.substr(leftSegmentStart, leftIndex - leftSegmentStart);
        const std::string rightSegment = right.substr(rightSegmentStart, rightIndex - rightSegmentStart);
        const int comparison = leftNumeric
            ? compare_numeric_strings(leftSegment, rightSegment)
            : (leftSegment == rightSegment ? 0 : (leftSegment < rightSegment ? -1 : 1));
        if (comparison != 0) {
            return comparison;
        }
    }

    return 0;
}

int compare_rpm_evr(const std::string& left, const std::string& right) {
    const RpmEvrValue parsedLeft = parse_rpm_evr(left);
    const RpmEvrValue parsedRight = parse_rpm_evr(right);
    if (parsedLeft.epoch != parsedRight.epoch) {
        return parsedLeft.epoch < parsedRight.epoch ? -1 : 1;
    }
    const int versionComparison = rpmvercmp(parsedLeft.version, parsedRight.version);
    if (versionComparison != 0) {
        return versionComparison;
    }
    return rpmvercmp(parsedLeft.release, parsedRight.release);
}

std::vector<std::string> tokenize_maven(const std::string& raw) {
    std::vector<std::string> tokens;
    std::string current;
    const std::string value = lower_copy(raw);
    auto flush = [&]() {
        if (!current.empty()) {
            tokens.push_back(current);
            current.clear();
        }
    };

    for (char c : value) {
        if (c == '.' || c == '-' || c == '_') {
            flush();
            continue;
        }
        if (!current.empty()) {
            const bool currentNumeric = std::isdigit(static_cast<unsigned char>(current.back())) != 0;
            const bool nextNumeric = std::isdigit(static_cast<unsigned char>(c)) != 0;
            if (currentNumeric != nextNumeric) {
                flush();
            }
        }
        current.push_back(c);
    }
    flush();
    return tokens;
}

std::pair<int, std::string> maven_qualifier_key(const std::string& token) {
    static const std::map<std::string, int> knownQualifiers{
        {"alpha", 1},
        {"a", 1},
        {"beta", 2},
        {"b", 2},
        {"milestone", 3},
        {"m", 3},
        {"rc", 4},
        {"cr", 4},
        {"snapshot", 5},
        {"", 6},
        {"ga", 6},
        {"final", 6},
        {"release", 6},
        {"sp", 7},
    };

    const std::string normalized = lower_copy(token);
    const auto it = knownQualifiers.find(normalized);
    if (it != knownQualifiers.end()) {
        return {it->second, {}};
    }
    return {50, normalized};
}

int compare_maven_token(const std::string& left, const std::string& right) {
    const bool leftNumeric = is_numeric_token(left);
    const bool rightNumeric = is_numeric_token(right);
    if (leftNumeric && rightNumeric) {
        return compare_numeric_strings(left.empty() ? "0" : left, right.empty() ? "0" : right);
    }
    if (left.empty() && right.empty()) {
        return 0;
    }
    if (left.empty() || right.empty()) {
        const std::string emptySide = left.empty() ? std::string{} : left;
        const std::string otherSide = left.empty() ? right : left;
        if (is_numeric_token(otherSide)) {
            const int comparison = compare_numeric_strings(emptySide.empty() ? "0" : emptySide, otherSide);
            return left.empty() ? comparison : -comparison;
        }
        const auto leftKey = maven_qualifier_key(left);
        const auto rightKey = maven_qualifier_key(right);
        if (leftKey.first != rightKey.first) {
            return leftKey.first < rightKey.first ? -1 : 1;
        }
        if (leftKey.second == rightKey.second) {
            return 0;
        }
        return leftKey.second < rightKey.second ? -1 : 1;
    }
    if (leftNumeric != rightNumeric) {
        return leftNumeric ? 1 : -1;
    }

    const auto leftKey = maven_qualifier_key(left);
    const auto rightKey = maven_qualifier_key(right);
    if (leftKey.first != rightKey.first) {
        return leftKey.first < rightKey.first ? -1 : 1;
    }
    if (leftKey.second == rightKey.second) {
        return 0;
    }
    return leftKey.second < rightKey.second ? -1 : 1;
}

int compare_maven(const std::string& left, const std::string& right) {
    const std::vector<std::string> leftTokens = tokenize_maven(left);
    const std::vector<std::string> rightTokens = tokenize_maven(right);
    const std::size_t maxSize = std::max(leftTokens.size(), rightTokens.size());
    for (std::size_t index = 0; index < maxSize; ++index) {
        const std::string leftToken = index < leftTokens.size() ? leftTokens[index] : std::string{};
        const std::string rightToken = index < rightTokens.size() ? rightTokens[index] : std::string{};
        const int comparison = compare_maven_token(leftToken, rightToken);
        if (comparison != 0) {
            return comparison;
        }
    }
    return 0;
}

std::vector<std::string> tokenize(const std::string& value, const VersionComparatorSpec& spec) {
    if (spec.profile == "lexicographic" || spec.profile == "raw") {
        return {spec.caseInsensitive ? lower_copy(value) : value};
    }

    const std::string pattern = spec.tokenPattern.empty() ? std::string{"[0-9]+|[A-Za-z]+"} : spec.tokenPattern;
    std::regex_constants::syntax_option_type options = std::regex_constants::ECMAScript;
    if (spec.caseInsensitive) {
        options |= std::regex_constants::icase;
    }

    const std::regex tokenRegex(pattern, options);
    std::vector<std::string> tokens;
    auto begin = std::sregex_iterator(value.begin(), value.end(), tokenRegex);
    auto end = std::sregex_iterator();
    for (auto it = begin; it != end; ++it) {
        const std::string token = it->str();
        tokens.push_back(spec.caseInsensitive ? lower_copy(token) : token);
    }

    if (tokens.empty()) {
        tokens.push_back(spec.caseInsensitive ? lower_copy(value) : value);
    }
    return tokens;
}

int compare_tokens(const std::string& left, const std::string& right) {
    const bool leftNumeric = is_numeric_token(left);
    const bool rightNumeric = is_numeric_token(right);
    if (leftNumeric && rightNumeric) {
        return compare_numeric_strings(left, right);
    }
    if (leftNumeric != rightNumeric) {
        return leftNumeric ? 1 : -1;
    }
    if (left == right) {
        return 0;
    }
    return left < right ? -1 : 1;
}

}  // namespace

int version_compare_values(
    const std::string& left,
    const std::string& right,
    const VersionComparatorSpec& spec
) {
    if (spec.profile == "semver") {
        return compare_semver(left, right);
    }
    if (spec.profile == "rpm-evr") {
        return compare_rpm_evr(left, right);
    }
    if (spec.profile == "pep440") {
        return compare_pep440(left, right);
    }
    if (spec.profile == "maven-comparable") {
        return compare_maven(left, right);
    }

    const std::vector<std::string> leftTokens = tokenize(left, spec);
    const std::vector<std::string> rightTokens = tokenize(right, spec);
    const std::size_t maxSize = std::max(leftTokens.size(), rightTokens.size());
    for (std::size_t index = 0; index < maxSize; ++index) {
        const std::string leftToken = index < leftTokens.size() ? leftTokens[index] : std::string{"0"};
        const std::string rightToken = index < rightTokens.size() ? rightTokens[index] : std::string{"0"};
        const int comparison = compare_tokens(leftToken, rightToken);
        if (comparison != 0) {
            return comparison;
        }
    }

    return 0;
}

bool version_less_equal(
    const std::string& left,
    const std::string& right,
    const VersionComparatorSpec& spec
) {
    return version_compare_values(left, right, spec) <= 0;
}

bool version_greater_equal(
    const std::string& left,
    const std::string& right,
    const VersionComparatorSpec& spec
) {
    return version_compare_values(left, right, spec) >= 0;
}
