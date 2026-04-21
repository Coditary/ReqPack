#pragma once

#include <boost/graph/adjacency_list.hpp>
#include <string>

enum class ActionType {
	INSTALL,
	REMOVE,
	UPDATE,
	SEARCH
};

struct Request {
	ActionType action;
	std::vector<std::string> packages;
	std::vector<std::string> flags;
};

struct Package {
	std::string system;
	std::string name;
	std::string version;
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

