cmake_minimum_required(VERSION 3.16)

project(mod_audio_fork
    VERSION 0.1.0
    DESCRIPTION "FreeSWITCH module: stream channel audio to a WebSocket server"
    LANGUAGES C CXX
)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)

pkg_check_modules(LIBWEBSOCKETS REQUIRED IMPORTED_TARGET libwebsockets)
pkg_check_modules(SPEEXDSP      REQUIRED IMPORTED_TARGET speexdsp)

# FreeSWITCH headers and library — override via -D or env vars when not in standard paths.
find_path(FREESWITCH_INCLUDE_DIR
    NAMES switch.h
    HINTS ENV FREESWITCH_INCLUDE_DIR
    PATHS
        /usr/include/freeswitch
        /usr/local/include/freeswitch
        /usr/local/freeswitch/include/freeswitch
        /opt/freeswitch/include/freeswitch
)
find_library(FREESWITCH_LIBRARY
    NAMES freeswitch
    HINTS ENV FREESWITCH_LIBRARY_DIR
    PATHS
        /usr/lib
        /usr/lib/x86_64-linux-gnu
        /usr/local/lib
        /usr/local/freeswitch/lib
        /opt/freeswitch/lib
)
if(NOT FREESWITCH_INCLUDE_DIR OR NOT FREESWITCH_LIBRARY)
    message(FATAL_ERROR
        "FreeSWITCH not found. Install freeswitch-dev, or pass -DFREESWITCH_INCLUDE_DIR=... -DFREESWITCH_LIBRARY=..."
    )
endif()

find_path(BOOST_CIRCULAR_BUFFER_INCLUDE_DIR boost/circular_buffer.hpp)
if(NOT BOOST_CIRCULAR_BUFFER_INCLUDE_DIR)
    message(FATAL_ERROR
        "boost/circular_buffer.hpp not found. Install libboost-dev (or your platform's equivalent)."
    )
endif()

add_library(mod_audio_fork MODULE
    mod_audio_fork.c
    lws_glue.cpp
    parser.cpp
    audio_pipe.cpp
    ws_uri.cpp
)

set_target_properties(mod_audio_fork PROPERTIES
    PREFIX ""
    OUTPUT_NAME mod_audio_fork
)

target_include_directories(mod_audio_fork PRIVATE
    ${FREESWITCH_INCLUDE_DIR}
    ${BOOST_CIRCULAR_BUFFER_INCLUDE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(mod_audio_fork PRIVATE
    ${FREESWITCH_LIBRARY}
    PkgConfig::LIBWEBSOCKETS
    PkgConfig::SPEEXDSP
    Threads::Threads
)

target_compile_options(mod_audio_fork PRIVATE -Wall -Wextra)

# ── unit tests ─────────────────────────────────────────────────────────────
#
# ws_uri.cpp has no FreeSWITCH / libwebsockets dependency, so its tests link
# against nothing and run in a couple of seconds. That is why the parser was
# split out of lws_glue.cpp — a translation unit that pulls in switch.h cannot
# be exercised without a whole FS install.
#
# ★ Built by default (it is cheap) so `cmake --build` cannot leave it stale,
#   and run by tests/smoke.sh.
add_executable(ws_uri_test tests/ws_uri_test.cpp ws_uri.cpp)
target_include_directories(ws_uri_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(ws_uri_test PRIVATE -Wall -Wextra)

# drop_throttle.hpp is header-only, so this links nothing at all.
add_executable(throttle_test tests/throttle_test.cpp)
target_include_directories(throttle_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(throttle_test PRIVATE -Wall -Wextra)

# json_escape.hpp is header-only too.
add_executable(json_escape_test tests/json_escape_test.cpp)
target_include_directories(json_escape_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(json_escape_test PRIVATE -Wall -Wextra)

# media_fill.hpp is header-only. ★ This is the first test in the suite that
# touches the read path at all — see tests/media_fill_test.cpp.
add_executable(media_fill_test tests/media_fill_test.cpp)
target_include_directories(media_fill_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(media_fill_test PRIVATE -Wall -Wextra)

set(FREESWITCH_MOD_DIR "/usr/local/freeswitch/mod"
    CACHE PATH "FreeSWITCH module install directory")

install(TARGETS mod_audio_fork
    LIBRARY DESTINATION ${FREESWITCH_MOD_DIR}
)

message(STATUS "")
message(STATUS "mod_audio_fork configuration")
message(STATUS "  Build type:            ${CMAKE_BUILD_TYPE}")
message(STATUS "  FreeSWITCH include:    ${FREESWITCH_INCLUDE_DIR}")
message(STATUS "  FreeSWITCH library:    ${FREESWITCH_LIBRARY}")
message(STATUS "  libwebsockets:         ${LIBWEBSOCKETS_VERSION}")
message(STATUS "  speexdsp:              ${SPEEXDSP_VERSION}")
message(STATUS "  Boost circular_buffer: ${BOOST_CIRCULAR_BUFFER_INCLUDE_DIR}")
message(STATUS "  Install destination:   ${FREESWITCH_MOD_DIR}")
message(STATUS "")
