set(RECOMP_VECTORS "${CMAKE_CURRENT_SOURCE_DIR}/vectors/arm.vec" CACHE FILEPATH "Reference vector file")
set(VECTORS ${RECOMP_VECTORS})

add_executable(recomp_genblocks gen_blocks.cpp)
target_link_libraries(recomp_genblocks PRIVATE recomp)

add_custom_command(
  OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/gen_blocks.c
  COMMAND recomp_genblocks ${VECTORS} ${CMAKE_CURRENT_BINARY_DIR}/gen_blocks.c
  DEPENDS recomp_genblocks ${VECTORS}
  COMMENT "Emitting C for the reference vectors")

add_executable(recomp_tests test_main.cpp ${CMAKE_CURRENT_BINARY_DIR}/gen_blocks.c)
target_include_directories(recomp_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(recomp_tests PRIVATE recomp)
add_test(NAME recomp_vectors COMMAND recomp_tests ${VECTORS})

# Emitted code on a real guest-memory reservation (fault reporting)
add_executable(recomp_genfault gen_fault_blocks.cpp)
target_link_libraries(recomp_genfault PRIVATE recomp)
add_custom_command(
  OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/fault_blocks.c
  COMMAND recomp_genfault ${CMAKE_CURRENT_BINARY_DIR}/fault_blocks.c
  DEPENDS recomp_genfault
  COMMENT "Emitting C for the fault test")
add_executable(recomp_fault_test fault_test.cpp ${CMAKE_CURRENT_BINARY_DIR}/fault_blocks.c)
target_link_libraries(recomp_fault_test PRIVATE pokerecomp_runtime)
add_test(NAME recomp_fault COMMAND recomp_fault_test)

# Dispatcher: block discovery -> emitted C -> runtime dispatcher with interpreter fallback
add_executable(recomp_gendispatch gen_dispatch.cpp)
target_link_libraries(recomp_gendispatch PRIVATE recomp)
add_custom_command(
  OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/dispatch_blocks.c
  COMMAND recomp_gendispatch ${CMAKE_CURRENT_BINARY_DIR}/dispatch_blocks.c
  DEPENDS recomp_gendispatch
  COMMENT "Discovering blocks and emitting C for the dispatcher test")
add_executable(recomp_dispatch_test dispatch_test.cpp ${CMAKE_CURRENT_BINARY_DIR}/dispatch_blocks.c)
target_link_libraries(recomp_dispatch_test PRIVATE pokerecomp_runtime)
add_test(NAME recomp_dispatch COMMAND recomp_dispatch_test)
