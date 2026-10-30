cmake_minimum_required(VERSION 3.25)
project(Vanilo VERSION 1.0 DESCRIPTION "Vanilo Tasker C++" LANGUAGES CXX)

if(PROJECT_SOURCE_DIR STREQUAL PROJECT_BINARY_DIR)
    message(FATAL_ERROR "In-source builds are not allowed. Use a preset (cmake --preset debug) or -B <build-dir>.")
endif()

# Tests, examples, strict warnings and install rules are for working on tasker itself. They are
# off by default when tasker is part of another project (FetchContent or add_subdirectory),
# which then only gets the Vanilo::Vanilo target and decides BUILD_SHARED_LIBS itself.
option(TASKER_BUILD_TESTS "Build the unit tests" ${PROJECT_IS_TOP_LEVEL})
option(TASKER_BUILD_EXAMPLES "Build the examples" ${PROJECT_IS_TOP_LEVEL})
option(TASKER_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ${PROJECT_IS_TOP_LEVEL})
option(TASKER_INSTALL "Generate install rules and a Vanilo CMake package" ${PROJECT_IS_TOP_LEVEL})
set(TASKER_SANITIZER "" CACHE STRING "Build tasker with a sanitizer: address (with UBSan), thread, or empty")
set_property(CACHE TASKER_SANITIZER PROPERTY STRINGS "" address thread)

if(PROJECT_IS_TOP_LEVEL)
    option(BUILD_SHARED_LIBS "Build Vanilo as a shared library" ON)

    if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
        set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    endif()

    # Executables in bin/, libraries in bin/lib/, tests in bin/tests/, with $ORIGIN-relative RPATHs.
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin")
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin/lib")
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin/lib")
    set(CMAKE_BUILD_RPATH_USE_ORIGIN ON)
endif()

if(TASKER_SANITIZER STREQUAL "address")
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
elseif(TASKER_SANITIZER STREQUAL "thread")
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
    add_link_options(-fsanitize=thread)
elseif(TASKER_SANITIZER)
    message(FATAL_ERROR "TASKER_SANITIZER must be address, thread or empty, not '${TASKER_SANITIZER}'.")
endif()

if(TASKER_BUILD_TESTS)
    enable_testing()
endif()

add_subdirectory(vanilo)

if(TASKER_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()
