cmake_minimum_required(VERSION 3.16)
project(njproto CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra)

add_library(njproto STATIC
  src/message.cpp
  src/sha1.cpp
  src/connection.cpp
  src/client.cpp
  src/server.cpp
)
target_include_directories(njproto PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

add_executable(njserver tools/njserver.cpp)
target_link_libraries(njserver njproto)

add_executable(njcli tools/njcli.cpp)
target_link_libraries(njcli njproto)

enable_testing()
foreach(t test_codec test_sha1 test_loopback)
  add_executable(${t} tests/${t}.cpp)
  target_link_libraries(${t} njproto)
  add_test(NAME ${t} COMMAND ${t})
endforeach()

# ---------------------------------------------------------------- engine
# The client engine (ENGINE.md) needs libopus, libogg and libFLAC.
option(NJPROTO_BUILD_ENGINE "Build the njproto_engine library, njjam and its tests" ON)
if(NJPROTO_BUILD_ENGINE)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(CODECS REQUIRED IMPORTED_TARGET opus ogg flac)

  add_library(njproto_engine STATIC
    engine/src/dsp.cpp
    engine/src/opus_codec.cpp
    engine/src/flac_codec.cpp
    engine/src/engine.cpp
  )
  target_include_directories(njproto_engine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
  target_link_libraries(njproto_engine PUBLIC njproto PkgConfig::CODECS)
  find_package(Threads REQUIRED)
  target_link_libraries(njproto_engine PUBLIC Threads::Threads)

  add_executable(njjam tools/njjam.cpp)
  target_link_libraries(njjam njproto_engine)

  foreach(t test_engine_codecs test_engine_clock test_engine_e2e)
    add_executable(${t} engine/tests/${t}.cpp)
    target_include_directories(${t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    target_link_libraries(${t} njproto_engine)
    add_test(NAME ${t} COMMAND ${t})
  endforeach()
endif()
