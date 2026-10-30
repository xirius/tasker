# Vanilo: the tasker library (Vanilo::Vanilo).

include(GNUInstallDirs)

find_package(Threads REQUIRED)

file(GLOB_RECURSE VANILO_SOURCES CONFIGURE_DEPENDS
    "src/*.cpp" "src/*.h" "src/*.hpp"
    "private/*.h" "private/*.hpp"
    "include/*.h" "include/*.hpp")

add_library(Vanilo ${VANILO_SOURCES})
add_library(Vanilo::Vanilo ALIAS Vanilo)

# C++17 is the minimum; consumers may build it with a newer standard.
target_compile_features(Vanilo PUBLIC cxx_std_17)
set_target_properties(Vanilo PROPERTIES
    CXX_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON  # a static Vanilo can be linked into shared libraries
    COMPILE_WARNING_AS_ERROR ${TASKER_WARNINGS_AS_ERRORS}
    DEBUG_POSTFIX ".d"
    MINSIZEREL_POSTFIX ".mr"
    RELWITHDEBINFO_POSTFIX ".rd")

target_include_directories(Vanilo
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>"  # generated export header
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/private"
        "${CMAKE_CURRENT_SOURCE_DIR}/src")

target_link_libraries(Vanilo PUBLIC Threads::Threads atomic)

include(GenerateExportHeader)
generate_export_header(Vanilo
    BASE_NAME Vanilo
    EXPORT_MACRO_NAME VANILO_EXPORT
    EXPORT_FILE_NAME _vanilo_export.h)

get_target_property(VANILO_TYPE Vanilo TYPE)
if(VANILO_TYPE STREQUAL "STATIC_LIBRARY")
    target_compile_definitions(Vanilo PUBLIC VANILO_STATIC_DEFINE)
endif()

# Unit tests
if(TASKER_BUILD_TESTS)
    include(FetchContent)
    FetchContent_Declare(Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG v3.16.1
        GIT_SHALLOW TRUE
        SYSTEM
        FIND_PACKAGE_ARGS 3 CONFIG)

    block()
        set(BUILD_SHARED_LIBS OFF)
        FetchContent_MakeAvailable(Catch2)
    endblock()

    file(GLOB_RECURSE VANILO_TEST_SOURCES CONFIGURE_DEPENDS "tests/*.cpp" "tests/*.h")

    add_executable(Vanilo.tests ${VANILO_TEST_SOURCES})
    target_link_libraries(Vanilo.tests PRIVATE Vanilo::Vanilo Catch2::Catch2WithMain)
    target_include_directories(Vanilo.tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/private"
        "${CMAKE_CURRENT_SOURCE_DIR}/src")
    set_target_properties(Vanilo.tests PROPERTIES
        CXX_EXTENSIONS OFF
        RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin/tests"
        COMPILE_WARNING_AS_ERROR ${TASKER_WARNINGS_AS_ERRORS})

    add_test(NAME Vanilo.tests COMMAND Vanilo.tests)
endif()

# Install rules and CMake package, so consumers can also use find_package(Vanilo)
if(TASKER_INSTALL)
    include(CMakePackageConfigHelpers)

    set(VANILO_CONFIG_DIR "${CMAKE_INSTALL_LIBDIR}/cmake/Vanilo")

    install(TARGETS Vanilo EXPORT VaniloTargets
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
        LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
    install(DIRECTORY include/ DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/_vanilo_export.h" DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
    install(EXPORT VaniloTargets NAMESPACE Vanilo:: DESTINATION "${VANILO_CONFIG_DIR}")

    configure_package_config_file("${PROJECT_SOURCE_DIR}/cmake/VaniloConfig.cmake.in"
        "${PROJECT_BINARY_DIR}/VaniloConfig.cmake"
        INSTALL_DESTINATION "${VANILO_CONFIG_DIR}")
    write_basic_package_version_file("${PROJECT_BINARY_DIR}/VaniloConfigVersion.cmake"
        COMPATIBILITY SameMajorVersion)
    install(FILES
        "${PROJECT_BINARY_DIR}/VaniloConfig.cmake"
        "${PROJECT_BINARY_DIR}/VaniloConfigVersion.cmake"
        DESTINATION "${VANILO_CONFIG_DIR}")
endif()
