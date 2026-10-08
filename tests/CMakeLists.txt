find_package(doctest CONFIG REQUIRED)

# Later test targets also link the reader_core static library (tasks/VERIFY.md).

add_executable(test_pdf_document test_pdf_document.cpp)
target_compile_definitions(test_pdf_document PRIVATE TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
target_link_libraries(test_pdf_document PRIVATE doctest::doctest reader_core)
add_test(NAME pdf_document COMMAND test_pdf_document)

add_executable(test_poppler test_poppler.cpp)
target_compile_definitions(test_poppler PRIVATE SAMPLE_PDF_PATH="${CMAKE_CURRENT_SOURCE_DIR}/data/sample.pdf")
target_link_libraries(test_poppler PRIVATE doctest::doctest reader_core)
add_test(NAME poppler COMMAND test_poppler)

add_executable(test_smoke test_smoke.cpp)
target_compile_features(test_smoke PRIVATE cxx_std_20)
target_link_libraries(test_smoke PRIVATE doctest::doctest)
add_test(NAME smoke COMMAND test_smoke)
