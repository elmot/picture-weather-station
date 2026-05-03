cmake_minimum_required(VERSION 3.21)
project(design-check CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

list(PREPEND CMAKE_PREFIX_PATH "${CMAKE_CURRENT_SOURCE_DIR}/slint-distribution")
find_package(Slint REQUIRED)

add_executable(design-generator
    design-generator.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../main/dithering.cpp)
target_include_directories(design-generator PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../main ${CMAKE_CURRENT_SOURCE_DIR}/../ui)
target_link_libraries(design-generator PRIVATE Slint::Slint)

slint_target_sources(design-generator
    ${CMAKE_CURRENT_SOURCE_DIR}/../ui/weather-station.slint)

set_source_files_properties(${UI_ASSETS} PROPERTIES HEADER_FILE_ONLY TRUE)
target_sources(design-generator PRIVATE ${UI_ASSETS})

# Copy the Slint shared library next to the executable so it can run from the
# build directory without needing the DLL on PATH.
add_custom_command(TARGET design-generator POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:slint_cpp-shared>
        $<TARGET_FILE_DIR:design-generator>)