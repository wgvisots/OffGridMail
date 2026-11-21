cmake_minimum_required(VERSION 3.22)
project(ogm VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug)
endif()

option(OGM_BUILD_TESTS "Build core unit tests" ON)
option(OGM_SANITIZE "Build with address and undefined-behavior sanitizers" OFF)

if(OGM_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

set(OGM_WARNINGS $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Wconversion -Wshadow>)

# ---- The portable core: protocol, link layer, mailbox.  No sockets, no clock.
add_library(ogmcore STATIC
  core/src/kiss.cpp
  core/src/ax25.cpp
  core/src/md5.cpp
  core/src/secure_login.cpp
  core/src/lzhuf.cpp
  core/src/message.cpp
  core/src/b2f.cpp
  core/src/ax25_link.cpp
  core/src/radio.cpp
  core/src/channel.cpp
  core/src/mailbox.cpp
  core/src/engine.cpp
)
target_include_directories(ogmcore PUBLIC core/include)
target_compile_options(ogmcore PRIVATE ${OGM_WARNINGS})
set_target_properties(ogmcore PROPERTIES POSITION_INDEPENDENT_CODE ON)

# ---- A pretend RMS gateway for tests and for ogmfake (also no I/O).
add_library(ogmsim STATIC sim/fake_rms.cpp)
target_include_directories(ogmsim PUBLIC sim)
target_link_libraries(ogmsim PUBLIC ogmcore)
target_compile_options(ogmsim PRIVATE ${OGM_WARNINGS})

if(OGM_BUILD_TESTS)
  enable_testing()
  add_executable(ogmcore_tests
    core/tests/main.cpp
    core/tests/test_kiss.cpp
    core/tests/test_ax25.cpp
    core/tests/test_lzhuf.cpp
    core/tests/test_message.cpp
    core/tests/test_b2f.cpp
    core/tests/test_ax25_link.cpp
    core/tests/test_engine.cpp
  )
  target_link_libraries(ogmcore_tests PRIVATE ogmcore ogmsim)
  target_compile_options(ogmcore_tests PRIVATE ${OGM_WARNINGS})
  target_compile_definitions(ogmcore_tests PRIVATE
    OGM_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
  add_test(NAME ogmcore_tests COMMAND ogmcore_tests)
endif()

# ---- Terminal tools (Linux): ogm drives the engine; ogmfake is a pretend RMS.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT ANDROID)
  add_executable(ogm apps/cli/main.cpp)
  target_link_libraries(ogm PRIVATE ogmcore)
  target_compile_options(ogm PRIVATE ${OGM_WARNINGS})
  find_path(OGM_BLUEZ_INCLUDE bluetooth/rfcomm.h)
  if(NOT OGM_BLUEZ_INCLUDE)
    message(STATUS "BlueZ headers not found: ogm is built without 'connect d75' (apt install libbluetooth-dev)")
  endif()

  add_executable(ogmfake apps/fake/main.cpp)
  target_link_libraries(ogmfake PRIVATE ogmsim)
  target_compile_options(ogmfake PRIVATE ${OGM_WARNINGS})
  install(TARGETS ogm RUNTIME DESTINATION bin)
endif()
