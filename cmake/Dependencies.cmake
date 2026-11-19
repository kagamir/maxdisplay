include(FetchContent)

# ENet 1.3.18. Upstream's static target does not export its include directory.
FetchContent_Declare(enet
    GIT_REPOSITORY https://github.com/lsalzman/enet.git
    GIT_TAG 2662c0de09e36f2a2030ccc2c528a3e4c9e8138a)
FetchContent_MakeAvailable(enet)
target_include_directories(enet INTERFACE "${enet_SOURCE_DIR}/include")
if(WIN32)
    target_link_libraries(enet INTERFACE winmm ws2_32)
endif()

if(APPLE OR WIN32)
    foreach(option ENABLE_APPS ENABLE_DOCS ENABLE_EXAMPLES ENABLE_TESTDATA ENABLE_TESTS ENABLE_TOOLS)
        set(${option} OFF CACHE BOOL "Build libaom components" FORCE)
    endforeach()
    set(BUILD_SHARED_LIBS OFF)
    set(CONFIG_REALTIME_ONLY 1 CACHE STRING "Use the realtime AV1 encoder" FORCE)
    set(CONFIG_AV1_HIGHBITDEPTH 0 CACHE STRING "Use 8-bit video" FORCE)
    set(CONFIG_SIZE_LIMIT 1 CACHE STRING "Limit AV1 decoder allocation" FORCE)
    set(DECODE_WIDTH_LIMIT 4096 CACHE STRING "Maximum coded width" FORCE)
    set(DECODE_HEIGHT_LIMIT 2304 CACHE STRING "Maximum coded height" FORCE)
    if(MSVC)
        set(ENABLE_NASM ON CACHE BOOL "Use NASM for x86 SIMD" FORCE)
        set(MSVC_RUNTIME static CACHE STRING "Match MaxDisplay's static CRT" FORCE)
    endif()
    FetchContent_Declare(libaom
        URL https://storage.googleapis.com/aom-releases/libaom-3.15.3.tar.gz
        URL_HASH SHA256=12c01dc731ed7b5a999e28120b45bf58a002ba3435a4ba2cfbc1e084a27c9c04)
    FetchContent_MakeAvailable(libaom)

    # Keep the standalone binaries self-contained, including their dependency notices.
    file(READ "${enet_SOURCE_DIR}/LICENSE" enet_license)
    set(notices "ENet 1.3.18\n${enet_license}\n\nlibaom 3.15.3\n")
    foreach(notice LICENSE PATENTS third_party/fastfeat/LICENSE third_party/vector/LICENSE third_party/x86inc/LICENSE)
        file(READ "${libaom_SOURCE_DIR}/${notice}" license_text)
        string(APPEND notices "\n${notice}\n${license_text}\n")
    endforeach()
    configure_file("${CMAKE_CURRENT_LIST_DIR}/third_party_notices.hpp.in"
                   "${PROJECT_BINARY_DIR}/generated/third_party_notices.hpp" @ONLY)
endif()
