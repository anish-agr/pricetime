add_executable(replay_itch replay_itch.cpp)
target_link_libraries(replay_itch PRIVATE pricetime::pricetime pricetime_warnings)

add_executable(gen_itch gen_itch.cpp)
target_link_libraries(gen_itch PRIVATE pricetime::pricetime pricetime_warnings)

add_executable(mm_sandbox mm_sandbox.cpp)
target_link_libraries(mm_sandbox PRIVATE pricetime::pricetime pricetime_warnings)
