include(FetchContent)

# Save the current value.
set(CURRENT_BUILD_SHARED_LIBS ${BUILD_SHARED_LIBS})

# Override it to build static libraries.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

# Set Catch2-specific options.
set(CATCH_BUILD_STATIC_LIBRARY ON CACHE BOOL "Build Catch2 as a static library" FORCE)

set(VERSION "v3.11.0")

FetchContent_Declare(
catch2
GIT_REPOSITORY https://github.com/catchorg/Catch2.git
GIT_TAG "${VERSION}"
)

FetchContent_MakeAvailable(catch2)

# Restore the original setting if needed.
set(BUILD_SHARED_LIBS ${CURRENT_BUILD_SHARED_LIBS} CACHE BOOL "" FORCE)

message(STATUS "Using catch2 ${VERSION}")
