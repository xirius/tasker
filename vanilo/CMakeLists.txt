# Tasker library
set(TARGET_NAME "Vanilo")

# Use POSIX threads
set(CMAKE_THREAD_PREFER_PTHREAD TRUE)
set(THREADS_PREFER_PTHREAD_FLAG TRUE)
find_package(Threads REQUIRED)

# Defining library
add_library("${TARGET_NAME}" "")
add_library("${TARGET_NAME}::${TARGET_NAME}" ALIAS "${TARGET_NAME}")

# Export header
include("GenerateExportHeader")
string(TOUPPER "${TARGET_NAME}_STATIC_DEFINE" TARGET_EXPORT_DEFINE)
string(TOLOWER "_${TARGET_NAME}_export.h" TARGET_EXPORT_FILE)

generate_export_header("${TARGET_NAME}"
    EXPORT_MACRO_NAME "${TARGET_EXPORT_DEFINE}"
    EXPORT_FILE_NAME "${TARGET_EXPORT_FILE}")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/${TARGET_EXPORT_FILE}" DESTINATION "include")

# Folder structure
set(PUBLIC_HEADER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/include")
set(PRIVATE_HEADER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/private")
set(SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src")

# Collect source files
file(GLOB_RECURSE SOURCES FOLLOW_SYMLINKS CONFIGURE_DEPENDS
    "${SOURCE_DIR}/*.cpp"
    "${SOURCE_DIR}/*.hpp"
    "${SOURCE_DIR}/*.h"
    "${PRIVATE_HEADER_DIR}/*.h"
    "${PRIVATE_HEADER_DIR}/*.hpp"
    "${PUBLIC_HEADER_DIR}/*h"
    "${PUBLIC_HEADER_DIR}/*hpp")

if(NOT BUILD_SHARED_LIBS)
    set_target_properties("${TARGET_NAME}" PROPERTIES COMPILE_FLAGS "-D${TARGET_EXPORT_DEFINE}")
    set_target_properties(${TARGET_NAME} PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

set_target_properties("${PROJECT_NAME}" PROPERTIES
    DEBUG_POSTFIX ".d"
    RELEASE_POSTFIX ""
    MINSIZEREL_POSTFIX ".mr"
    RELWITHDEBINFO_POSTFIX ".rd")

# Set RPATH
set_target_properties(${TARGET_NAME} PROPERTIES LINK_FLAGS "-Wl,-rpath,./ -Wl,-rpath,../${LIBRARY_FOLDER}")

# Set common compile options
target_compile_options(${TARGET_NAME} PRIVATE -Werror)

# Add include directories of private and public headers
target_include_directories(${TARGET_NAME}
    PUBLIC "$<INSTALL_INTERFACE:include>"
    PUBLIC $<BUILD_INTERFACE:${PUBLIC_HEADER_DIR}>
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
    PRIVATE $<BUILD_INTERFACE:${PRIVATE_HEADER_DIR}>
    PRIVATE $<BUILD_INTERFACE:${SOURCE_DIR}>)

target_sources(${TARGET_NAME} PRIVATE ${SOURCES})

# Build Unit Tests
if(BUILD_UNIT_TESTS)
    set(TARGET_UT_NAME "${TARGET_NAME}.tests")

    # Adjust unit test binary output folder
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${OUTPUT_DIRECTORY}/${UT_BIN_FOLDER})

    # Collect unit test source files
    file(GLOB_RECURSE UT_SOURCES FOLLOW_SYMLINKS CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp")

    # Remove main.cpp from sources
    list(FILTER SOURCES EXCLUDE REGEX ".*/main.cpp$")

    # Create unit test executable
    add_executable(${TARGET_UT_NAME} ${UT_SOURCES} ${SOURCES})

    # Set common compile options
    target_compile_options(${TARGET_UT_NAME} PRIVATE -Werror)

    # Retrieve the module's included directories
    get_target_property(INCLUDE_DIRS ${TARGET_NAME} INCLUDE_DIRECTORIES)

    # Set include directories of private and public headers
    target_include_directories(${TARGET_UT_NAME} PRIVATE ${INCLUDE_DIRS})

    # Set common compile features
    target_compile_features(${TARGET_UT_NAME} PRIVATE "cxx_std_${CMAKE_CXX_STANDARD}")

    # Set RPATH
    set_target_properties(${TARGET_UT_NAME} PROPERTIES LINK_FLAGS "-Wl,-rpath,./ -Wl,-rpath,../${LIBRARY_FOLDER}")

    # Add Catch2 framework
    target_link_libraries(${TARGET_UT_NAME} PRIVATE Catch2::Catch2)
endif()
