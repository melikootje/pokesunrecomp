add_executable(romlib_tests test_main.cpp)
target_link_libraries(romlib_tests PRIVATE romlib)
add_test(NAME romlib COMMAND romlib_tests)

add_executable(mkfixture mkfixture.cpp)
target_link_libraries(mkfixture PRIVATE romlib)
add_test(NAME romtool_cli
  COMMAND ${CMAKE_COMMAND} -DMKFIXTURE=$<TARGET_FILE:mkfixture> -DROMTOOL=$<TARGET_FILE:romtool>
          -DWORKDIR=${CMAKE_CURRENT_BINARY_DIR}/cli -P ${CMAKE_CURRENT_SOURCE_DIR}/cli_test.cmake)
