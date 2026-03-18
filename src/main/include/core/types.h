#pragma once

#include <boost/graph/adjacency_list.hpp>

struct Package {
	std::string system;
	std::string name;
	std::string version;
};

typedef boost::adjacency_list<boost::vecS, boost::vecS, boost::directedS, Package> Graph;

