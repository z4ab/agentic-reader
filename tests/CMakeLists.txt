find_package(doctest CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

# Later test targets also link the reader_core static library (tasks/VERIFY.md).

# A bash script that greps the source tree (needs python3 for the sidecar part).
if(NOT WIN32)
    add_test(NAME branding COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/test_branding.sh ${PROJECT_SOURCE_DIR})
endif()

add_executable(test_error_ux test_error_ux.cpp)
target_link_libraries(test_error_ux PRIVATE doctest::doctest reader_core)
add_test(NAME error_ux COMMAND test_error_ux)

add_executable(test_markdown test_markdown.cpp)
target_link_libraries(test_markdown PRIVATE doctest::doctest reader_core)
add_test(NAME markdown COMMAND test_markdown)

add_executable(test_open_files test_open_files.cpp)
target_compile_definitions(test_open_files PRIVATE TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
target_link_libraries(test_open_files PRIVATE doctest::doctest reader_core)
add_test(NAME open_files COMMAND test_open_files)

add_executable(test_page_cache test_page_cache.cpp)
target_link_libraries(test_page_cache PRIVATE doctest::doctest reader_core)
add_test(NAME page_cache COMMAND test_page_cache)

add_executable(test_page_layout test_page_layout.cpp)
target_link_libraries(test_page_layout PRIVATE doctest::doctest reader_core)
add_test(NAME page_layout COMMAND test_page_layout)

add_executable(test_pdf_document test_pdf_document.cpp)
target_compile_definitions(test_pdf_document PRIVATE TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
target_link_libraries(test_pdf_document PRIVATE doctest::doctest reader_core)
add_test(NAME pdf_document COMMAND test_pdf_document)

add_executable(test_pdf_view_nav test_pdf_view_nav.cpp)
target_link_libraries(test_pdf_view_nav PRIVATE doctest::doctest reader_core)
add_test(NAME pdf_view_nav COMMAND test_pdf_view_nav)

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

add_executable(test_protocol_cpp test_protocol_cpp.cpp)
target_compile_definitions(test_protocol_cpp PRIVATE PROTOCOL_MD_PATH="${PROJECT_SOURCE_DIR}/sidecar/PROTOCOL.md")
target_link_libraries(test_protocol_cpp PRIVATE doctest::doctest nlohmann_json::nlohmann_json reader_core)
add_test(NAME protocol_cpp COMMAND test_protocol_cpp)

add_executable(test_settings test_settings.cpp)
target_link_libraries(test_settings PRIVATE doctest::doctest reader_core)
add_test(NAME settings COMMAND test_settings)

# The fake sidecar is a POSIX shell script.
if(NOT WIN32)
    add_executable(test_sidecar_client test_sidecar_client.cpp)
    target_link_libraries(test_sidecar_client PRIVATE doctest::doctest reader_core)
    add_test(NAME sidecar_client COMMAND test_sidecar_client)
endif()

add_executable(test_sidecar_env test_sidecar_env.cpp)
target_link_libraries(test_sidecar_env PRIVATE doctest::doctest reader_core)
add_test(NAME sidecar_env COMMAND test_sidecar_env)

if(NOT WIN32)
    add_executable(test_sidecar_restart test_sidecar_restart.cpp)
    target_link_libraries(test_sidecar_restart PRIVATE doctest::doctest reader_core)
    add_test(NAME sidecar_restart COMMAND test_sidecar_restart)
endif()

if(NOT WIN32)
    add_executable(test_sidecar_shutdown test_sidecar_shutdown.cpp)
    target_link_libraries(test_sidecar_shutdown PRIVATE doctest::doctest reader_core)
    add_test(NAME sidecar_shutdown COMMAND test_sidecar_shutdown)
endif()

add_executable(test_smoke test_smoke.cpp)
target_compile_features(test_smoke PRIVATE cxx_std_20)
target_link_libraries(test_smoke PRIVATE doctest::doctest)
add_test(NAME smoke COMMAND test_smoke)

add_executable(test_text_layer test_text_layer.cpp)
target_compile_definitions(test_text_layer PRIVATE TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
target_link_libraries(test_text_layer PRIVATE doctest::doctest reader_core)
add_test(NAME text_layer COMMAND test_text_layer)

add_executable(test_tool_format test_tool_format.cpp)
target_link_libraries(test_tool_format PRIVATE doctest::doctest reader_core)
add_test(NAME tool_format COMMAND test_tool_format)
