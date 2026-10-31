include(FetchContent)

FetchContent_Declare(benchmark
    GIT_REPOSITORY https://github.com/google/benchmark.git
    GIT_TAG v1.10.0
    GIT_SHALLOW TRUE
    SYSTEM
    FIND_PACKAGE_ARGS CONFIG)

block()
    set(BUILD_SHARED_LIBS OFF)
    set(BENCHMARK_ENABLE_TESTING OFF)
    set(BENCHMARK_ENABLE_GTEST_TESTS OFF)
    set(BENCHMARK_ENABLE_INSTALL OFF)
    set(BENCHMARK_ENABLE_WERROR OFF)
    FetchContent_MakeAvailable(benchmark)
endblock()

add_executable(Vanilo.benchmarks TaskerBenchmarks.cpp)
target_link_libraries(Vanilo.benchmarks PRIVATE Vanilo::Vanilo benchmark::benchmark)
target_compile_options(Vanilo.benchmarks PRIVATE ${TASKER_WARNING_FLAGS})
set_target_properties(Vanilo.benchmarks PROPERTIES
    CXX_EXTENSIONS OFF
    COMPILE_WARNING_AS_ERROR ${TASKER_WARNINGS_AS_ERRORS})
