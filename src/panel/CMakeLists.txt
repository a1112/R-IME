find_package(PkgConfig REQUIRED)
pkg_check_modules(CAIRO REQUIRED IMPORTED_TARGET cairo pangocairo)
add_library(rime-ime-panel STATIC renderer.cpp layout.cpp)
target_link_libraries(rime-ime-panel PUBLIC PkgConfig::CAIRO nlohmann_json::nlohmann_json)
