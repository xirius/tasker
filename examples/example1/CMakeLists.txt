set(TARGET "Example01")

add_executable("${TARGET}" "main.cpp")
target_link_libraries("${TARGET}" PRIVATE Vanilo::Vanilo)
set_target_properties("${TARGET}" PROPERTIES LINK_FLAGS "-Wl,-rpath,./ -Wl,-rpath,../lib")
