add_executable(Example01 main.cpp)
target_link_libraries(Example01 PRIVATE Vanilo::Vanilo)
set_target_properties(Example01 PROPERTIES
    CXX_EXTENSIONS OFF
    COMPILE_WARNING_AS_ERROR ${TASKER_WARNINGS_AS_ERRORS})
