#pragma once

#include <string>
#include <vector>
#include "core/types.h"

class IPlugin {
public:
    virtual ~IPlugin() = default;

	virtual void init() = 0;
	virtual void shutdown() = 0;

    virtual std::string getName() const = 0;
    virtual std::string getVersion() const = 0;
    
    virtual std::vector<Package> getRequirements() = 0;
	virtual std::vector<string> getCategories() = 0;

    virtual void install(const std::vector<Package>& packages) = 0;
    virtual void remove(const std::vector<Package>& packages) = 0;
    virtual void update(const std::vector<Package>& packages) = 0;

    virtual std::vector<PackageInfo> list() = 0;
    
    virtual std::vector<PackageInfo> search(const std::string& prompt) = 0;
    
    virtual PackageInfo info(const std::string& packageName) = 0;
};
