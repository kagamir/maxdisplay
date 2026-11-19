set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>" CACHE STRING "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
# Upstream ENABLE_DECODER controls its player executable, the library always builds.
set(ENABLE_DECODER OFF CACHE BOOL "" FORCE)
set(ENABLE_SDL OFF CACHE BOOL "" FORCE)
