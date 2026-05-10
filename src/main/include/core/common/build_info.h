#pragma once

#include <string>

#ifndef REQPACK_RELEASE_ID
#define REQPACK_RELEASE_ID "dev"
#endif

inline std::string reqpack_build_release_id() {
    return REQPACK_RELEASE_ID;
}

inline std::string reqpack_user_agent() {
    return std::string{"ReqPack/"} + reqpack_build_release_id();
}
