if(NOT TARGET nlohmann_json::nlohmann_json)
  find_package(nlohmann_json 3.10 REQUIRED)
endif()
add_library(rime-ime-core STATIC config.cpp)
set_target_properties(rime-ime-core PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_features(rime-ime-core PUBLIC cxx_std_20)
target_include_directories(rime-ime-core PUBLIC
  $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(rime-ime-core PUBLIC nlohmann_json::nlohmann_json)
if(WIN32)
  target_link_libraries(rime-ime-core PUBLIC shell32 ole32)
endif()
