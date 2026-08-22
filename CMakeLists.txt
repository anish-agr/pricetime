cmake_minimum_required(VERSION 3.21)

project(pricetime
  VERSION 0.1.0
  DESCRIPTION "Price-time-priority limit order book and matching engine"
  LANGUAGES CXX)

option(PRICETIME_BUILD_TESTS "Build unit and property tests" ON)
option(PRICETIME_BUILD_BENCH "Build latency benchmarks" ON)
option(PRICETIME_BUILD_TOOLS "Build command-line tools" ON)
option(PRICETIME_WERROR "Treat warnings as errors" OFF)
option(PRICETIME_SANITIZE "Build with address and undefined-behavior sanitizers" OFF)
option(PRICETIME_TSAN "Build with the thread sanitizer" OFF)

# Core: header-only, zero external dependencies.
add_library(pricetime INTERFACE)
add_library(pricetime::pricetime ALIAS pricetime)
target_include_directories(pricetime INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(pricetime INTERFACE cxx_std_20)

# Warning flags for our own translation units (not exported to consumers).
add_library(pricetime_warnings INTERFACE)
if(MSVC)
  target_compile_options(pricetime_warnings INTERFACE /W4 /permissive-)
  if(PRICETIME_WERROR)
    target_compile_options(pricetime_warnings INTERFACE /WX)
  endif()
else()
  target_compile_options(pricetime_warnings INTERFACE -Wall -Wextra -Wpedantic)
  if(PRICETIME_WERROR)
    target_compile_options(pricetime_warnings INTERFACE -Werror)
  endif()
endif()

if(PRICETIME_SANITIZE)
  if(MSVC)
    message(FATAL_ERROR "PRICETIME_SANITIZE is only wired up for GCC/Clang")
  endif()
  # The tests replace global operator new/delete; ASan must be told to stand
  # aside there rather than intercepting, which it does automatically for
  # replaced operators. UBSan runs with trap-on-error so CI fails loudly.
  target_compile_options(pricetime_warnings INTERFACE
    -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
  target_link_options(pricetime_warnings INTERFACE -fsanitize=address,undefined)
endif()

if(PRICETIME_TSAN)
  if(MSVC)
    message(FATAL_ERROR "PRICETIME_TSAN is only wired up for GCC/Clang")
  endif()
  # ASan and TSan cannot be combined, so this is a separate build.
  # The SPSC queue is the only lock-free code here and the only thing whose
  # correctness a single-threaded test genuinely cannot establish.
  target_compile_options(pricetime_warnings INTERFACE -fsanitize=thread -g)
  target_link_options(pricetime_warnings INTERFACE -fsanitize=thread)
endif()

if(PRICETIME_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(PRICETIME_BUILD_BENCH)
  add_subdirectory(bench)
endif()

if(PRICETIME_BUILD_TOOLS)
  add_subdirectory(tools)
endif()
