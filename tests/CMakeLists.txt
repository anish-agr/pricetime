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
  test_primitives.cpp
  test_book.cpp
  test_matching.cpp
  test_properties.cpp)

target_link_libraries(pricetime_tests
  PRIVATE pricetime::pricetime pricetime_warnings doctest::doctest)

add_test(NAME pricetime_tests COMMAND pricetime_tests)
