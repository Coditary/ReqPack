#pragma once

#include <string>

struct VersionComparatorSpec {
    std::string profile{};
    std::string tokenPattern{};
    bool caseInsensitive{true};
};

int version_compare_values(
    const std::string& left,
    const std::string& right,
    const VersionComparatorSpec& spec = {}
);

bool version_less_equal(
    const std::string& left,
    const std::string& right,
    const VersionComparatorSpec& spec = {}
);

bool version_greater_equal(
    const std::string& left,
    const std::string& right,
    const VersionComparatorSpec& spec = {}
);
