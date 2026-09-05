# Built only when PRICETIME_FUZZ=ON, which requires a compiler with libFuzzer
# (clang). Everything the fuzzer exercises is also reachable from the normal
# test binary through tests/test_fuzz_corpus.cpp, so nothing here is needed to
# get coverage of the parser on other toolchains.
if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  message(FATAL_ERROR "PRICETIME_FUZZ requires clang (libFuzzer); got ${CMAKE_CXX_COMPILER_ID}")
endif()

add_executable(fuzz_itch fuzz_itch.cpp)
target_include_directories(fuzz_itch PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(fuzz_itch PRIVATE pricetime::pricetime pricetime_warnings)

# The sanitizers are the point: libFuzzer generates the inputs, ASan and UBSan
# are what turn a bad read into a report instead of a wrong number.
target_compile_options(fuzz_itch PRIVATE
  -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
target_link_options(fuzz_itch PRIVATE -fsanitize=fuzzer,address,undefined)
