#pragma once

#include <boost/graph/adjacency_list.hpp>
#include <optional>
#include <string>
#include <utility>
#include <vector>

enum class ActionType {
	UNKNOWN,
	INSTALL,
	REMOVE,
	UPDATE,
	SEARCH,
	LIST,
	INFO,
	ENSURE,
	SBOM,
	AUDIT,
	OUTDATED,
	HOST,
	SNAPSHOT,
	SERVE,
	REMOTE
};

struct Request {
	ActionType action{ActionType::UNKNOWN};
	std::string system;
	std::vector<std::string> packages;
	std::vector<std::string> flags;
	std::string outputFormat;
	std::string outputPath;
	std::string localPath;
	bool usesLocalTarget{false};
};

struct ProxyResolution {
	std::string targetSystem;
	std::optional<std::vector<std::string>> packages;
	std::optional<std::string> localPath;
	std::optional<std::vector<std::string>> flags;
};

struct Package {
	ActionType action{ActionType::UNKNOWN};
	std::string system;
	std::string name;
	std::string version;
	std::string sourcePath;
	bool localTarget{false};
	std::vector<std::string> flags;
	bool directRequest{false};
};

struct PackageInfo {
	std::string system;
	std::string name;
	std::string packageId;
	std::string version;
	std::string latestVersion;
	std::string status;
	std::string installed;
	std::string summary;
	std::string description;
	std::string homepage;
	std::string documentation;
	std::string sourceUrl;
	std::string repository;
	std::string channel;
	std::string section;
	std::string packageType;
	std::string architecture;
	std::string license;
	std::string author;
	std::string maintainer;
	std::string email;
	std::string publishedAt;
	std::string updatedAt;
	std::string size;
	std::string installedSize;
	std::vector<std::string> dependencies;
	std::vector<std::string> optionalDependencies;
	std::vector<std::string> provides;
	std::vector<std::string> conflicts;
	std::vector<std::string> replaces;
	std::vector<std::string> binaries;
	std::vector<std::string> tags;
	std::vector<std::pair<std::string, std::string>> extraFields;
};


typedef boost::adjacency_list<boost::vecS, boost::vecS, boost::directedS, Package> Graph;
