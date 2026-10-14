cmake_minimum_required(VERSION 3.15)
project(MemGlow VERSION 0.1.0 LANGUAGES CXX)

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "MemGlow requires a 64-bit Windows build.")
endif()
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>")
set(SDL2_ROOT "" CACHE PATH "SDL2 development package")
if(NOT SDL2_ROOT AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../work/sdl2-sdk/SDL2-2.32.10")
    set(SDL2_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../work/sdl2-sdk/SDL2-2.32.10")
endif()
find_package(SDL2 CONFIG REQUIRED COMPONENTS SDL2)

add_library(memglow_core STATIC
    src/full_memory_view.cpp
    src/memory_types.cpp
    src/viewer_options.cpp
)
target_compile_features(memglow_core PUBLIC cxx_std_17)
target_include_directories(memglow_core PUBLIC include PRIVATE src)
target_compile_definitions(memglow_core PRIVATE WIN32 NOMINMAX PSAPI_VERSION=1 _CRT_SECURE_NO_WARNINGS)
target_link_libraries(memglow_core PRIVATE SDL2::SDL2 psapi)

add_executable(memglow src/main.cpp)
set_target_properties(memglow PROPERTIES OUTPUT_NAME "MemGlow")
target_link_libraries(memglow PRIVATE memglow_core)

# Both executables use this runtime directory; copy the dependency once per build.
add_custom_target(memglow_runtime
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_BINARY_DIR}/bin/$<CONFIG>"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:SDL2::SDL2>" "${CMAKE_BINARY_DIR}/bin/$<CONFIG>"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/third_party/SDL2-LICENSE.txt" "${CMAKE_BINARY_DIR}/bin/$<CONFIG>/SDL2-LICENSE.txt"
)
add_dependencies(memglow memglow_runtime)

include(CTest)
if(BUILD_TESTING)
    add_executable(memglow_tests tests/self_test_main.cpp)
    target_link_libraries(memglow_tests PRIVATE memglow_core)
    add_dependencies(memglow_tests memglow_runtime)
    add_test(NAME memglow_regression COMMAND memglow_tests)
endif()

install(TARGETS memglow RUNTIME DESTINATION .)
install(FILES
    "$<TARGET_FILE:SDL2::SDL2>"
    LICENSE.md
    README.md
    README.zh-CN.md
    DESTINATION .
)
install(FILES third_party/SDL2-LICENSE.txt DESTINATION third_party)
install(DIRECTORY docs/images DESTINATION docs)

set(CPACK_PACKAGE_NAME "MemGlow")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_FILE_NAME "MemGlow-${PROJECT_VERSION}-windows-x64")
set(CPACK_GENERATOR ZIP)
include(CPack)
