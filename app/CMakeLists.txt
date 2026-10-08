find_package(nlohmann_json CONFIG REQUIRED)
find_package(unofficial-poppler CONFIG REQUIRED)
find_package(wxWidgets CONFIG REQUIRED)

# The vcpkg poppler target exports include/poppler. poppler-cpp headers live in
# include/poppler/cpp and are included as <poppler-document.h> (poppler-cpp.pc layout).
find_path(POPPLER_CPP_INCLUDE_DIR poppler-document.h PATH_SUFFIXES poppler/cpp REQUIRED)

# Everything in app/src except main.cpp. Tests link this library too.
add_library(reader_core STATIC
    src/MainFrame.cpp
    src/pdf/PageLayout.cpp
    src/pdf/PdfDocument.cpp
    src/pdf/PdfView.cpp
    src/project/Project.cpp
)
target_include_directories(reader_core PUBLIC src)
target_include_directories(reader_core SYSTEM PUBLIC ${POPPLER_CPP_INCLUDE_DIR})
target_compile_features(reader_core PUBLIC cxx_std_20)
target_link_libraries(reader_core PUBLIC nlohmann_json::nlohmann_json unofficial::poppler::poppler-cpp wx::core wx::base)

add_executable(agentic-reader src/main.cpp)
target_link_libraries(agentic-reader PRIVATE reader_core)
