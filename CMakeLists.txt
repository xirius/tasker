cmake_minimum_required(VERSION 3.17)
project(Vanilo VERSION 1.0 DESCRIPTION "Vanilo Tasker C++" LANGUAGES CXX)

if(${CMAKE_VERSION} VERSION_LESS 3.17)
    cmake_policy(VERSION ${CMAKE_MAJOR_VERSION}.${CMAKE_MINOR_VERSION})
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Options
option(BUILD_SHARED_LIBS "Build shared library [default: true]." True)
option(BUILD_UNIT_TESTS "Build unit tests [default: true]" True)
option(BUILD_EXAMPLES "Build examples [default: true]" True)

# Building with cmake verbose
set(CMAKE_VERBOSE_MAKEFILE OFF)

# Git is required
find_package(Git REQUIRED)

# Set module path
set(CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)
set(CMAKE_PREFIX_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)

# cmake scripts
include(Environment)

# Add modules
add_subdirectory(extern)
add_subdirectory(vanilo)
add_subdirectory(examples)
