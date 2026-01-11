function(check_system NAME RESULT)
    string(TOLOWER ${CMAKE_SYSTEM_NAME} SYSTEM_NAME)
    string(TOLOWER ${NAME} NAME)

    if(${SYSTEM_NAME} MATCHES ${NAME})
        set(${RESULT} TRUE PARENT_SCOPE)
    else()
        set(${RESULT} FALSE PARENT_SCOPE)
    endif()
endfunction(check_system)

# Set environment variables
check_system("linux" IS_LINUX)
check_system("windows" IS_WINDOWS)

# Build types
set(VALID_BUILD_TYPES "Release" "Debug" "MinSizeRel" "RelWithDebInfo" "Asan" "Tsan")

if(NOT CMAKE_CONFIGURATION_TYPES)
    if("${CMAKE_BUILD_TYPE}" STREQUAL "")
        set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build configuration" FORCE)
    endif()

    if(DEFINED CMAKE_BUILD_TYPE)
        set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS ${VALID_BUILD_TYPES})
    endif()
endif()

# Validate build type
if(NOT CMAKE_CONFIGURATION_TYPES)
    list(FIND VALID_BUILD_TYPES "${CMAKE_BUILD_TYPE}" INDEX)
    if(${INDEX} MATCHES -1)
        message(FATAL_ERROR "Invalid build type. Valid types are [${VALID_BUILD_TYPES}]")
    endif()
endif()

if(BUILD_TYPE MATCHES "relwithdebinfo")
    set(BUILD_TYPE "release")
endif()

# Setup output binary dir
set(OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

# Output folders
set(BINARY_FOLDER "bin")
set(LIBRARY_FOLDER "lib")
set(ARCHIVE_FOLDER "lib")
set(UT_BIN_FOLDER "${BINARY_FOLDER}/tests") # Unit Test

# Full output directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${OUTPUT_DIRECTORY}/${BINARY_FOLDER})
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${OUTPUT_DIRECTORY}/${BINARY_FOLDER}/${ARCHIVE_FOLDER})
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${OUTPUT_DIRECTORY}/${BINARY_FOLDER}/${LIBRARY_FOLDER})

# Relative linking path settings
set(CMAKE_SKIP_BUILD_RPATH TRUE)
set(CMAKE_BUILD_WITH_INSTALL_RPATH FALSE)
set(CMAKE_INSTALL_RPATH_USE_LINK_PATH FALSE)

# Disable in-source builds to prevent source tree corruption
if("${CMAKE_SOURCE_DIR}" STREQUAL "${CMAKE_BINARY_DIR}")
    message(FATAL_ERROR " FATAL: In-source builds are not allowed.
        You should create a separate directory for build files.")
endif()

# Require out-of-source builds
file(TO_CMAKE_PATH "${PROJECT_BINARY_DIR}/CMakeLists.txt" LOC_PATH)
if(EXISTS "${LOC_PATH}")
    message(FATAL_ERROR "You cannot build in a source directory (or any directory with a CMakeLists.txt file).
        Please make a build subdirectory. Feel free to remove CMakeCache.txt and CMakeFiles.")
endif()
