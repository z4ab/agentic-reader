find_package(doctest CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

# Later test targets also link the reader_core static library (tasks/VERIFY.md).

add_executable(test_pdf_document test_pdf_document.cpp)
target_compile_definitions(test_pdf_document PRIVATE TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
target_link_libraries(test_pdf_document PRIVATE doctest::doctest reader_core)
add_test(NAME pdf_document COMMAND test_pdf_document)

add_executable(test_poppler test_poppler.cpp)
target_compile_definitions(test_poppler PRIVATE SAMPLE_PDF_PATH="${CMAKE_CURRENT_SOURCE_DIR}/data/sample.pdf")
target_link_libraries(test_poppler PRIVATE doctest::doctest reader_core)
add_test(NAME poppler COMMAND test_poppler)

add_executable(test_project test_project.cpp)
target_link_libraries(test_project PRIVATE doctest::doctest nlohmann_json::nlohmann_json reader_core)
add_test(NAME project COMMAND test_project)

add_executable(test_project_transcript test_project_transcript.cpp)
target_link_libraries(test_project_transcript PRIVATE doctest::doctest reader_core)
add_test(NAME project_transcript COMMAND test_project_transcript)

add_executable(test_smoke test_smoke.cpp)
target_compile_features(test_smoke PRIVATE cxx_std_20)
target_link_libraries(test_smoke PRIVATE doctest::doctest)
add_test(NAME smoke COMMAND test_smoke)
