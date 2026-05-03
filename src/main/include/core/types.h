#pragma once

#include <boost/graph/adjacency_list.hpp>
#include <optional>
#include <string>
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
};

struct PackageInfo {
	std::string name;
	std::string version;
	std::string description;
	std::string homepage;
	std::string author;
	std::string email;
};


typedef boost::adjacency_list<boost::vecS, boost::vecS, boost::directedS, Package> Graph;
