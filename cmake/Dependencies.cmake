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
    if(WIN32)
        # The two codec projects both create a global 'dist' target. Build
        # libde265 in its own CMake project, with matching CRT and configuration.
        FetchContent_Declare(libde265
            GIT_REPOSITORY https://github.com/strukturag/libde265.git
            GIT_TAG ba62bf4cfb3242f3bf0a45617ff09e35236e4d82
            SOURCE_SUBDIR maxdisplay-populate-only)
        FetchContent_MakeAvailable(libde265)
        include(ExternalProject)
        set(de265_output "${libde265_BINARY_DIR}/lib")
        set(de265_archive "libde265${CMAKE_STATIC_LIBRARY_SUFFIX}")
        set(de265_path "${de265_output}/${de265_archive}")
        if(CMAKE_CONFIGURATION_TYPES)
            set(de265_path "${de265_output}/$<CONFIG>/${de265_archive}")
        endif()
        ExternalProject_Add(build_de265
            SOURCE_DIR "${libde265_SOURCE_DIR}" BINARY_DIR "${libde265_BINARY_DIR}"
            DOWNLOAD_COMMAND "" UPDATE_COMMAND "" INSTALL_COMMAND ""
            CMAKE_ARGS -C "${CMAKE_CURRENT_LIST_DIR}/de265-cache.cmake"
                       "-DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}" "-DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}"
                       "-DCMAKE_ARCHIVE_OUTPUT_DIRECTORY=${de265_output}"
            BUILD_COMMAND "${CMAKE_COMMAND}" --build <BINARY_DIR> --config "$<CONFIG>" --target de265 --parallel
            BUILD_BYPRODUCTS "${de265_path}")
        add_library(de265 STATIC IMPORTED GLOBAL)
        set_target_properties(de265 PROPERTIES IMPORTED_LOCATION "${de265_output}/${de265_archive}")
        foreach(config IN LISTS CMAKE_CONFIGURATION_TYPES)
            string(TOUPPER "${config}" config_upper)
            set_target_properties(de265 PROPERTIES IMPORTED_LOCATION_${config_upper} "${de265_output}/${config}/${de265_archive}")
        endforeach()
        add_dependencies(de265 build_de265)
        file(READ "${libde265_SOURCE_DIR}/COPYING" de265_license)
        string(APPEND notices "\nlibde265 1.1.3 - Copyright struktur AG and contributors. LGPL-3.0-or-later.\n"
               "The Windows release includes a source and relinking kit.\n${de265_license}")
    endif()
    # MSVC limits individual string literals. Split at line boundaries.
    string(REPLACE "\n" ")MaxDisplay\" \"\\n\"\nR\"MaxDisplay(" notices "${notices}")
    configure_file("${CMAKE_CURRENT_LIST_DIR}/third_party_notices.hpp.in"
                   "${PROJECT_BINARY_DIR}/generated/third_party_notices.hpp" @ONLY)
endif()
