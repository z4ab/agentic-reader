find_package(doctest CONFIG REQUIRED)

# Later test targets also link the reader_core static library (tasks/VERIFY.md).

add_executable(test_smoke test_smoke.cpp)
target_compile_features(test_smoke PRIVATE cxx_std_20)
target_link_libraries(test_smoke PRIVATE doctest::doctest)
add_test(NAME smoke COMMAND test_smoke)
