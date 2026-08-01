add_executable(pricetime_bench bench_book.cpp)

target_link_libraries(pricetime_bench
  PRIVATE pricetime::pricetime pricetime_warnings)

find_package(Threads REQUIRED)
target_link_libraries(pricetime_bench PRIVATE Threads::Threads)
