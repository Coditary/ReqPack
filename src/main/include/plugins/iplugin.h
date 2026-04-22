#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include "core/types.h"

#define REQPACK_API_VERSION 2

class IPlugin {
public:
    virtual ~IPlugin() = default;
	virtual uint32_t getInterfaceVersion() const {
        return REQPACK_API_VERSION;
    }

	virtual bool init() = 0;
	virtual bool shutdown() = 0;

    virtual std::string getName() const = 0;
    virtual std::string getVersion() const = 0;
    
	virtual std::vector<Package> getRequirements() = 0;
	virtual std::vector<std::string> getCategories() = 0;
	virtual std::vector<Package> getMissingPackages(const std::vector<Package>& packages) = 0;

    virtual void install(const std::vector<Package>& packages) = 0;
    virtual void remove(const std::vector<Package>& packages) = 0;
    virtual void update(const std::vector<Package>& packages) = 0;

    virtual std::vector<PackageInfo> list() = 0;
    
    virtual std::vector<PackageInfo> search(const std::string& prompt) = 0;
    
    virtual PackageInfo info(const std::string& packageName) = 0;
};
