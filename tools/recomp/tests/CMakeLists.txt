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
