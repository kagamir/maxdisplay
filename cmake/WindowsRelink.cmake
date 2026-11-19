# Assemble the LGPL relink materials and verify the recipe against the exact release objects.
set(kit "${BUILD_ROOT}/relink-kit")
set(verification "${BUILD_ROOT}/relink-verify")
file(REMOVE_RECURSE "${kit}" "${verification}")
file(MAKE_DIRECTORY "${kit}/objects" "${kit}/libraries")
string(REPLACE "|" ";" objects "${APP_OBJECTS}")
string(REPLACE "|" ";" libraries "${APP_LIBRARIES}")
file(COPY ${objects} DESTINATION "${kit}/objects")
file(COPY ${libraries} DESTINATION "${kit}/libraries")
file(COPY "${HEVC_SOURCE}/" DESTINATION "${kit}/libde265" PATTERN ".git" EXCLUDE)
configure_file("${CMAKE_CURRENT_LIST_DIR}/relink/CMakeLists.txt" "${kit}/CMakeLists.txt" COPYONLY)
configure_file("${CMAKE_CURRENT_LIST_DIR}/relink/README.txt" "${kit}/README.txt" COPYONLY)
execute_process(COMMAND "${CMAKE_COMMAND}" -E tar cf "${BUILD_ROOT}/maxdisplay-windows-relink.zip" --format=zip -- .
                WORKING_DIRECTORY "${kit}" COMMAND_ERROR_IS_FATAL ANY)

set(platform)
if(PLATFORM)
    list(APPEND platform -A "${PLATFORM}")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" -S "${kit}" -B "${verification}" -G "${GENERATOR}" ${platform}
                COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND "${CMAKE_COMMAND}" --build "${verification}" --config "${CONFIG}" --target maxdisplay --parallel
                COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND "${verification}/bin/maxdisplay.exe" --help OUTPUT_QUIET COMMAND_ERROR_IS_FATAL ANY)
