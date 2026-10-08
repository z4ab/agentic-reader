find_package(wxWidgets CONFIG REQUIRED)

# Everything in app/src except main.cpp. Tests link this library too.
add_library(reader_core STATIC
    src/MainFrame.cpp
)
target_include_directories(reader_core PUBLIC src)
target_compile_features(reader_core PUBLIC cxx_std_20)
target_link_libraries(reader_core PUBLIC wx::core wx::base)

add_executable(agentic-reader src/main.cpp)
target_link_libraries(agentic-reader PRIVATE reader_core)
