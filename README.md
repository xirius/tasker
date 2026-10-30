# Vanilo Tasker

A modern, simple C++17 task runner library and playground. This repository contains the Vanilo C++ library (Vanilo::Vanilo), unit tests powered by Catch2, and a minimal example application. It is built with CMake presets.

Current status: library and tests build and run; example target compiles. Some documentation items are still TODO and flagged below.


## Stack and Targets
- Language: C++17 (minimum; consumers may build it with a newer standard)
- Build system: CMake (>= 3.25) with presets
- Dependencies: system Threads/atomic; Catch2 v3 for the tests (found with find_package, otherwise fetched)
- Primary targets:
  - Library: Vanilo (alias: Vanilo::Vanilo)
  - Tests executable: Vanilo.tests (Catch2-based)
  - Example executable: Example01

CMake options (configure-time). The TASKER_* options default to ON only when tasker is the top-level project,
so a project that includes it with FetchContent or add_subdirectory just gets the Vanilo::Vanilo target:
- TASKER_BUILD_TESTS: Build the unit tests
- TASKER_BUILD_EXAMPLES: Build the examples
- TASKER_WARNINGS_AS_ERRORS: Treat compiler warnings as errors
- TASKER_INSTALL: Generate install rules and the Vanilo CMake package
- TASKER_SANITIZER: `address` (with UBSan), `thread`, or empty [default: empty]
- BUILD_SHARED_LIBS: Build Vanilo as a shared library [default when top-level: ON; otherwise the consumer decides]


## Overview
Vanilo is an experimental task runner with concurrency primitives. It currently exposes a tasker API and supporting concurrency utilities. The test suite exercises task submission and cancellation flows. The example application shows how to link against the library.

Key entry points:
- Library headers under vanilo/include/vanilo
- Example main: examples/example1/main.cpp
- Tests: vanilo/tests/src/*.cpp


## Requirements
- CMake >= 3.25 and Ninja (the presets use the Ninja generator)
- A C++17-compliant compiler (e.g., GCC 9+, Clang 10+, MSVC 2019+)
- Git (to fetch Catch2 when it isn't installed)
- POSIX threads and atomic support (Threads::Threads and atomic are linked)


## Build and Run
The presets build into `build/<preset>`: `debug`, `release`, `asan` (AddressSanitizer + UBSan) and `tsan` (ThreadSanitizer).

```sh
# Configure, build and run the tests in one go
cmake --workflow --preset debug

# Or step by step
cmake --preset debug
cmake --build --preset debug
ctest --preset debug

# Run the example
./build/debug/bin/Example01
```

### Using tasker from another project
```cmake
include(FetchContent)
FetchContent_Declare(tasker
    GIT_REPOSITORY https://github.com/xirius/tasker.git
    GIT_TAG <tag>
    FIND_PACKAGE_ARGS NAMES Vanilo CONFIG)
FetchContent_MakeAvailable(tasker)
target_link_libraries(my_target PRIVATE Vanilo::Vanilo)
```
Set `BUILD_SHARED_LIBS` around `FetchContent_MakeAvailable` (for example in a `block()`) to choose a static or shared Vanilo.
An installed tasker (`cmake --install build/release --prefix <prefix>`) is found with `find_package(Vanilo CONFIG)`.


## Scripts
No project-specific scripts are needed: the CMake presets cover building, testing and the sanitizer runs.


## Environment Variables
There are currently no required environment variables for building or running.
- TODO: document any runtime tracing/logging vars if/when introduced (e.g., for vanilo::core::Tracer).


## Tests
- Test target: Vanilo.tests, registered with CTest
- Framework: Catch2 v3
- Location: vanilo/tests/src/*.cpp and test config in vanilo/tests/_unit_test_config.cpp

```sh
ctest --preset debug                      # all tests
./build/debug/bin/tests/Vanilo.tests      # or run directly
./build/debug/bin/tests/Vanilo.tests "[ConcurrentQueue]"
cmake --workflow --preset tsan            # under ThreadSanitizer
```


## Project Structure
- CMakeLists.txt                 # Root build config and options
- CMakePresets.json              # debug, release, asan and tsan presets
- cmake/VaniloConfig.cmake.in    # Template for the installed Vanilo CMake package
- examples/
  - example1/
    - CMakeLists.txt
    - main.cpp                   # Example01 entry point
- vanilo/
  - CMakeLists.txt               # Library, tests and install rules
  - include/vanilo/              # Public headers
  - private/vanilo/              # Private headers/impl details
  - src/vanilo/                  # Library sources
  - tests/
    - _unit_test_config.cpp
    - src/*.cpp                  # Catch2 test cases
- LICENSE                        # MIT License
- README.md                      # This file


## License
This project is licensed under the MIT License. See the LICENSE file for details.


## Contributing
- TODO: add contribution guidelines and code style notes if external contributions are expected.
