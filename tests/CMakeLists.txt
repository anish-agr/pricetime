include(FetchContent)

# doctest's own CMakeLists declares an old minimum version; this keeps
# CMake >= 4.0 willing to configure it.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

FetchContent_Declare(doctest
  GIT_REPOSITORY https://github.com/doctest/doctest.git
  GIT_TAG        v2.4.12
  GIT_SHALLOW    TRUE)
set(DOCTEST_WITH_TESTS OFF CACHE BOOL "" FORCE)
set(DOCTEST_NO_INSTALL ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(doctest)

add_executable(pricetime_tests
  test_main.cpp
  alloc_counter.cpp
  test_primitives.cpp
  test_book.cpp
  test_matching.cpp
  test_order_types.cpp
  test_id_map.cpp
  test_properties.cpp
  test_reference_diff.cpp
  test_alloc.cpp
  test_itch.cpp
  test_itch_replay.cpp
  test_spsc.cpp
  test_market_maker.cpp
  test_engine.cpp
  test_queue_position.cpp
  test_moldudp64.cpp
  test_itch_stream.cpp
  test_ladder_pooled.cpp)

find_package(Threads REQUIRED)

target_link_libraries(pricetime_tests
  PRIVATE pricetime::pricetime pricetime_warnings doctest::doctest Threads::Threads)

add_test(NAME pricetime_tests COMMAND pricetime_tests)
