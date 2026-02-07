cmake_minimum_required(VERSION 3.21)

project(RadioSFSEPlugin LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(RadioSFSEPlugin SHARED
    src/logger.cpp
    src/papyrus_bridge.cpp
    src/plugin.cpp
    src/radio_engine.cpp
)

target_include_directories(RadioSFSEPlugin PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/sfse-0.2.18"
)

target_compile_definitions(RadioSFSEPlugin PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
)

target_link_libraries(RadioSFSEPlugin PRIVATE
    ole32
    shell32
    winmm
)

if(MSVC)
    target_link_options(RadioSFSEPlugin PRIVATE
        "/DEF:${CMAKE_CURRENT_SOURCE_DIR}/exports.def"
    )
endif()

set_target_properties(RadioSFSEPlugin PROPERTIES
    OUTPUT_NAME "RadioSFSE"
)
