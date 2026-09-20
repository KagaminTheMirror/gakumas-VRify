cmake_minimum_required(VERSION 4.0)
project(gakumas_VRify LANGUAGES C CXX ASM_MASM RC)
set(VR_COMPILER_PROCESSES 4 CACHE STRING "Maximum parallel compiler processes")
if(NOT MSVC OR NOT MSVC_TOOLSET_VERSION STREQUAL "143")
    message(FATAL_ERROR "gakumas-VRify requires Visual Studio 2022 (v143)")
endif()
get_filename_component(PROJECT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
set(ROOT "${PROJECT_ROOT}/build/source")
if(NOT EXISTS "${ROOT}/src/stdinclude.hpp")
    message(FATAL_ERROR "Run build.ps1 to fetch upstream and prepare the combined source tree")
endif()
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
foreach(dep cpprestsdk minizip ZLIB nlohmann_json fmt)
    find_package(${dep} CONFIG REQUIRED)
endforeach()
# Expose the resolved header path to the standalone config integration harness.
file(WRITE "${CMAKE_BINARY_DIR}/nlohmann-json-include.txt" "${nlohmann_json_INCLUDE_DIRS}")

file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS "${ROOT}/src/*.cpp" "${ROOT}/src/*.rc")
list(FILTER SOURCES EXCLUDE REGEX "\\.inc\\.cpp$")
file(GLOB_RECURSE MINHOOK_SOURCES CONFIGURE_DEPENDS "${ROOT}/deps/minhook/src/*.c")
add_library(minhook STATIC ${MINHOOK_SOURCES})
target_include_directories(minhook PUBLIC "${ROOT}/deps/minhook/include")

find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_custom_target(verify_staged_sources
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_ROOT}/scripts/source-pipeline.py" verify
    VERBATIM)

add_library(version SHARED ${SOURCES}
    "${ROOT}/src/dllproxy/version.asm"
    "${ROOT}/src/dllproxy/version.def")
add_dependencies(version verify_staged_sources)
add_custom_command(TARGET version POST_BUILD
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_ROOT}/scripts/source-pipeline.py" record-binary --binary "$<TARGET_FILE:version>"
    VERBATIM)
target_include_directories(version PRIVATE
    "${ROOT}/src" "${ROOT}/src/imgui" "${ROOT}/deps/rapidjson/include"
    "${ROOT}/deps/openxr/include")
target_compile_definitions(version PRIVATE GAKUMAS_EXTERNAL_HOST=1 IMGUI_USER_CONFIG="host/ImGuiUserConfig.hpp")
target_compile_options(version PRIVATE
    "$<$<COMPILE_LANGUAGE:CXX>:/std:c++latest;/utf-8;/MP${VR_COMPILER_PROCESSES};/Zi>"
    "$<$<AND:$<COMPILE_LANGUAGE:CXX>,$<CONFIG:Release>>:/O2;/Os>")
target_link_options(version PRIVATE /DEBUG /INCREMENTAL:NO)
target_link_libraries(version PRIVATE minhook cpprestsdk::cpprest minizip::minizip
    ZLIB::ZLIB nlohmann_json::nlohmann_json fmt::fmt d3d11 d3dcompiler dxgi advapi32)
set_target_properties(version PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${PROJECT_ROOT}/build/bin/x64/$<CONFIG>"
    ARCHIVE_OUTPUT_DIRECTORY "${PROJECT_ROOT}/build/bin/x64/$<CONFIG>"
    PDB_OUTPUT_DIRECTORY "${PROJECT_ROOT}/build/bin/x64/$<CONFIG>")
