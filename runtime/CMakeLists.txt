add_library(pokerecomp_runtime STATIC
  src/runtime.cpp src/guest_memory.cpp src/vfp.c
  src/decode.cpp src/interp.cpp src/dispatcher.cpp src/kernel.cpp src/services.cpp)
target_include_directories(pokerecomp_runtime PUBLIC include)
if(POKE_BUILD_TESTS AND NOT ANDROID)
  add_executable(guest_memory_test tests/guest_memory_test.cpp)
  target_link_libraries(guest_memory_test PRIVATE pokerecomp_runtime)
  add_test(NAME guest_memory COMMAND guest_memory_test)
  add_executable(kernel_test tests/kernel_test.cpp)
  target_link_libraries(kernel_test PRIVATE pokerecomp_runtime)
  add_test(NAME kernel COMMAND kernel_test)
endif()
