add_library(rime-ime-control STATIC control.cpp)
target_link_libraries(rime-ime-control PUBLIC rime-ime-core PRIVATE rime-ime-ipc)
target_include_directories(rime-ime-control PUBLIC
  $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_compile_features(rime-ime-control PUBLIC cxx_std_20)
add_library(RIME::control ALIAS rime-ime-control)
add_executable(r-ime-ctl cli.cpp)
target_link_libraries(r-ime-ctl PRIVATE rime-ime-control)
target_compile_features(r-ime-ctl PRIVATE cxx_std_20)
