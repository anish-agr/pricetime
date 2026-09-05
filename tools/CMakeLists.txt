add_executable(replay_itch replay_itch.cpp)
target_link_libraries(replay_itch PRIVATE pricetime::pricetime pricetime_warnings)

add_executable(gen_itch gen_itch.cpp)
target_link_libraries(gen_itch PRIVATE pricetime::pricetime pricetime_warnings)

add_executable(book_stats book_stats.cpp)
target_link_libraries(book_stats PRIVATE pricetime::pricetime pricetime_warnings)

add_executable(mm_sandbox mm_sandbox.cpp)
target_link_libraries(mm_sandbox PRIVATE pricetime::pricetime pricetime_warnings)

find_package(Threads REQUIRED)

add_executable(engine_server engine_server.cpp)
target_link_libraries(engine_server PRIVATE pricetime::pricetime pricetime_warnings Threads::Threads)

add_executable(engine_client engine_client.cpp)
target_link_libraries(engine_client PRIVATE pricetime::pricetime pricetime_warnings Threads::Threads)
