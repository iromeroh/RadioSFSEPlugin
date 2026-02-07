cmake_minimum_required(VERSION 3.21)

project(RadioSFSEPlugin LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(COMMONLIB_SHARED_SOURCES
    external/commonlibsf/lib/commonlib-shared/src/REL/IDDB.cpp
    external/commonlibsf/lib/commonlib-shared/src/REL/Module.cpp
    external/commonlibsf/lib/commonlib-shared/src/REL/Offset2ID.cpp
    external/commonlibsf/lib/commonlib-shared/src/REL/Utility.cpp
    external/commonlibsf/lib/commonlib-shared/src/REL/Version.cpp
    external/commonlibsf/lib/commonlib-shared/src/REX/REX.cpp
    external/commonlibsf/lib/commonlib-shared/src/REX/W32.cpp
)

set(COMMONLIB_RE_SOURCES
    external/commonlibsf/src/RE/A/Array.cpp
    external/commonlibsf/src/RE/O/Object.cpp
    external/commonlibsf/src/RE/T/TypeInfo.cpp
    external/commonlibsf/src/RE/V/Variable.cpp
)

add_library(RadioSFSEPlugin SHARED
    src/logger.cpp
    src/commonlib_rex_log.cpp
    src/papyrus_bridge.cpp
    src/plugin.cpp
    src/radio_engine.cpp
    ${COMMONLIB_SHARED_SOURCES}
    ${COMMONLIB_RE_SOURCES}
)

target_include_directories(RadioSFSEPlugin PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/sfse-0.2.18"
    "${CMAKE_CURRENT_SOURCE_DIR}/external/commonlibsf/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/external/commonlibsf/lib/commonlib-shared/include"
)

target_compile_definitions(RadioSFSEPlugin PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
)

target_link_libraries(RadioSFSEPlugin PRIVATE
    advapi32
    bcrypt
    d3d11
    d3dcompiler
    dbghelp
    dxgi
    ole32
    shell32
    version
    winmm
    ws2_32
)

if(MSVC)
    target_compile_options(RadioSFSEPlugin PRIVATE
        /bigobj
        /permissive-
        /FI"SFSE/Impl/PCH.h"
    )
    target_link_options(RadioSFSEPlugin PRIVATE
        "/DEF:${CMAKE_CURRENT_SOURCE_DIR}/exports.def"
    )
endif()

set_target_properties(RadioSFSEPlugin PROPERTIES
    OUTPUT_NAME "RadioSFSE"
)
