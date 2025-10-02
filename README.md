# Vanilo Tasker

A modern, simple C++17 task runner library and playground. This repository contains the Vanilo C++ library (Vanilo::Vanilo), unit tests powered by Catch2, and a minimal example application. It is built with CMake and uses Git to fetch the Catch2 test framework.

Current status: library and tests build and run; example target compiles. Some documentation items are still TODO and flagged below.


## Stack and Targets
- Language: C++17
- Build system: CMake (>= 3.17)
- Package/dependency management: CMake FetchContent for Catch2; system Threads/atomic
- Test framework: Catch2 v3 (fetched automatically)
- Primary targets:
  - Library: Vanilo (alias: Vanilo::Vanilo)
  - Tests executable: Vanilo.tests (Catch2-based)
  - Example executable: Example01

CMake options (configure-time):
- BUILD_SHARED_LIBS: Build shared library [default: ON]
- BUILD_UNIT_TESTS: Build unit tests [default: ON]
- BUILD_EXAMPLES: Build examples [default: ON]


## Overview
Vanilo is an experimental task runner with concurrency primitives. It currently exposes a tasker API and supporting concurrency utilities. The test suite exercises task submission and cancellation flows. The example application shows how to link against the library.

Key entry points:
- Library headers under vanilo/include/vanilo
- Example main: examples/example1/main.cpp
- Tests: vanilo/tests/src/*.cpp


## Requirements
- CMake >= 3.17
- A C++17-compliant compiler (e.g., GCC 9+, Clang 10+, MSVC 2019+)
- Git (required by the build to fetch Catch2)
- POSIX threads and atomic support (Threads::Threads and atomic are linked)

On Linux, make sure build-essential/clang, cmake, and git are installed. On Windows, use Visual Studio 2019+ with CMake integration.


## Build and Run
You can build with standard CMake, or use the provided CLion profiles described below.

### Standard CMake (any environment)
```sh
# From repository root
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug \
      -DBUILD_UNIT_TESTS=ON -DBUILD_EXAMPLES=ON
cmake --build build --target Vanilo.tests
# Run tests
./build/bin/tests/Vanilo.tests

# Build and run the example
cmake --build build --target Example01
./build/bin/Example01
```

### CLion/Preconfigured Profiles
This repository already has CLion build directories with profiles:
- Debug: /mnt/storage/dev/tasker/build-debug
- Release: /mnt/storage/dev/tasker/build-release

Example commands using those profiles:
```sh
# Build and run tests (Debug)
cmake --build /mnt/storage/dev/tasker/build-debug --target Vanilo.tests && \
/mnt/storage/dev/tasker/build-debug/bin/tests/Vanilo.tests

# Build and run example (Debug)
cmake --build /mnt/storage/dev/tasker/build-debug --target Example01 && \
/mnt/storage/dev/tasker/build-debug/bin/Example01
```
The same layout applies for Release under build-release.


## Scripts
No project-specific scripts are present at this time. All tasks are handled via CMake targets.
- TODO: add convenience scripts (e.g., scripts/test.sh) if needed in the future.


## Environment Variables
There are currently no required environment variables for building or running.
- TODO: document any runtime tracing/logging vars if/when introduced (e.g., for vanilo::core::Tracer).


## Tests
- Test target: Vanilo.tests
- Framework: Catch2 v3 (fetched during configure via FetchContent)
- Location: vanilo/tests/src/*.cpp and test config in vanilo/tests/_unit_test_config.cpp

Run all tests with:
```sh
# Using a generic build dir
./build/bin/tests/Vanilo.tests

# Or with CLion Debug profile
/mnt/storage/dev/tasker/build-debug/bin/tests/Vanilo.tests
```

You can pass Catch2 arguments after a `--` separator. Examples:
```sh
./build/bin/tests/Vanilo.tests --success
./build/bin/tests/Vanilo.tests [TaskRunner]
```


## Project Structure
- CMakeLists.txt                 # Root build config and options
- cmake/                         # CMake helper modules (e.g., Environment)
- extern/catch/                  # FetchContent configuration for Catch2
- examples/
  - example1/
    - CMakeLists.txt
    - main.cpp                   # Example01 entry point
- vanilo/
  - CMakeLists.txt               # Library + tests wiring
  - include/vanilo/              # Public headers
  - private/vanilo/              # Private headers/impl details
  - src/vanilo/                  # Library sources
  - tests/
    - _unit_test_config.cpp
    - src/*.cpp                  # Catch2 test cases
- build-debug/, build-release/   # Existing CLion build trees (generated)
- LICENSE                        # MIT License
- README.md                      # This file


## Installation and Use
This project currently focuses on development and testing. Installation rules for headers are partially defined (export header is installed), but a complete install/package configuration is not finalized.
- TODO: add proper install targets and packaging (e.g., CPack or export targets) when ready.

Consumers can link against the target Vanilo::Vanilo from the build tree for now.


## License
This project is licensed under the MIT License. See the LICENSE file for details.


## Contributing
- TODO: add contribution guidelines and code style notes if external contributions are expected.
