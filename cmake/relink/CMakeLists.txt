cmake_minimum_required(VERSION 3.20)
project(MaxDisplayRelink LANGUAGES C CXX)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(ENABLE_DECODER OFF CACHE BOOL "" FORCE)
set(ENABLE_SDL OFF CACHE BOOL "" FORCE)
add_subdirectory(libde265 EXCLUDE_FROM_ALL)
file(GLOB app_objects CONFIGURE_DEPENDS objects/*.obj)
file(GLOB app_libraries CONFIGURE_DEPENDS libraries/*.lib)
add_executable(maxdisplay ${app_objects})
set_target_properties(maxdisplay PROPERTIES LINKER_LANGUAGE CXX
                      RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<0:>")
target_link_libraries(maxdisplay PRIVATE ${app_libraries} de265 mfplat mfuuid wmcodecdspuuid
                      ole32 d3d11 dxgi dxguid user32 gdi32 iphlpapi ws2_32 winmm)
