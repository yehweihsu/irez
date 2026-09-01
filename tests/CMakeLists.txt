add_executable(irez-tests test_core.cpp)
target_link_libraries(irez-tests PRIVATE irez-core GTest::gtest_main)
target_compile_definitions(irez-tests PRIVATE
  IREZ_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../fixtures")
add_test(NAME irez-tests COMMAND irez-tests)

# Exact V00_01 contract test: replay the fixed command sequence through the
# C++ CLI and compare against the checked-in golden.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME diff-golden
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/../scripts/diff_test.py
          --binary $<TARGET_FILE:irez>)
add_test(NAME diff-normalize
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_diff_normalize.py)
