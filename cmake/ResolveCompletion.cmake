# Resolves Coditary Shared-Cpp completion (tab-completion utilities).
#
# Override:
#   cmake -DCOMPLETION_DIR=/path/to/Shared-Cpp/completion ...

function(resolve_completion)
    if(TARGET completion)
        return()
    endif()

    if(NOT COMPLETION_DIR)
        set(_completion_candidates
            "$ENV{HOME}/Dev/Coditary/shared/Shared-Cpp/completion"
            "${CMAKE_SOURCE_DIR}/../../Coditary/shared/Shared-Cpp/completion"
            "${CMAKE_SOURCE_DIR}/../../../Coditary/shared/Shared-Cpp/completion"
        )
        foreach(candidate IN LISTS _completion_candidates)
            if(EXISTS "${candidate}/CMakeLists.txt")
                set(COMPLETION_DIR "${candidate}" CACHE PATH "Path to Shared-Cpp/completion" FORCE)
                break()
            endif()
        endforeach()
    endif()

    if(NOT COMPLETION_DIR OR NOT EXISTS "${COMPLETION_DIR}/CMakeLists.txt")
        message(FATAL_ERROR
            "Shared-Cpp completion not found. Clone Coditary/shared or set -DCOMPLETION_DIR=...")
    endif()

    add_subdirectory("${COMPLETION_DIR}" "${CMAKE_BINARY_DIR}/_deps/completion-build")
endfunction()
