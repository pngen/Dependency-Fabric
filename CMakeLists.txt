cmake_minimum_required(VERSION 3.22)

project(dependency_fabric
        VERSION 1.0.0
        DESCRIPTION "Open-source, vendor-neutral C++20 dependency-graph runtime for heterogeneous AI infrastructure"
        HOMEPAGE_URL "https://github.com/pngen/Dependency-Fabric"
        LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(DF_BUILD_TESTS        "Build Dependency Fabric tests"                    ON)
option(DF_BUILD_BENCHMARKS   "Build Dependency Fabric benchmarks"               ON)
option(DF_BUILD_EXAMPLES     "Build Dependency Fabric examples"                 ON)
option(DF_BUILD_TOOLS        "Build Dependency Fabric CLI tool"                 ON)
option(DF_BUILD_CUDA         "Build the CUDA dependency-gated proof (sm_120)"   OFF)
option(DF_BUILD_SHARED       "Build Dependency Fabric as a shared library"      OFF)
option(DF_ENABLE_SANITIZERS  "Enable AddressSanitizer / undefined-behavior"     OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Default to Release if not specified.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# Core library sources
# ---------------------------------------------------------------------------
set(DF_CORE_SOURCES
    src/graph/graph_core.cpp
    src/graph/readiness.cpp
    src/graph/invalidation.cpp
    src/graph/recovery.cpp
    src/graph/enums.cpp
    src/persistence/format.cpp
    src/adapters/reference.cpp)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/protocol/framing.cpp")
  list(APPEND DF_CORE_SOURCES src/protocol/framing.cpp)
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/coordinator/coordinator.cpp")
  list(APPEND DF_CORE_SOURCES src/coordinator/coordinator.cpp)
endif()

set(DF_INSTALL_TARGETS "")

# ---------------------------------------------------------------------------
# Sanitizers (applied to every C++ translation unit, so a static library and its
# consumers stay ABI-consistent). MSVC supports /fsanitize=address; the ASan
# runtime is auto-linked by the compiler. Scoped to CXX so CUDA is unaffected.
# ---------------------------------------------------------------------------
if(DF_ENABLE_SANITIZERS)
  if(MSVC)
    add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:/fsanitize=address>")
  else()
    add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:-fsanitize=address>")
    add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:-fsanitize=undefined>")
    add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:-fno-omit-frame-pointer>")
    add_link_options("-fsanitize=address")
    add_link_options("-fsanitize=undefined")
  endif()
endif()

# ---------------------------------------------------------------------------
# The library
# ---------------------------------------------------------------------------
add_library(dependency_fabric ${DF_CORE_SOURCES})
add_library(DependencyFabric::DependencyFabric ALIAS dependency_fabric)
target_include_directories(dependency_fabric
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(dependency_fabric PUBLIC Threads::Threads)
set_target_properties(dependency_fabric PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    EXPORT_NAME "DependencyFabric")
if(DF_BUILD_SHARED)
  set_target_properties(dependency_fabric PROPERTIES VERSION ${PROJECT_VERSION} SOVERSION 1)
endif()

# ---------------------------------------------------------------------------
# Warnings: strict, /W4 /WX on MSVC, -Wall -Wextra -Wpedantic -Werror elsewhere.
# ---------------------------------------------------------------------------
if(MSVC)
  target_compile_options(dependency_fabric PRIVATE /W4 /WX)
  target_compile_definitions(dependency_fabric PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
  target_compile_options(dependency_fabric PRIVATE
      -Wall -Wextra -Wpedantic -Werror
      -Wshadow -Wconversion -Wsign-conversion -Wold-style-cast -Wdouble-promotion)
endif()

# Strict warnings for ALL C++ targets (library, tests, tools, examples, benchmarks).
# CUDA (.cu) files are compiled as language CUDA and are intentionally excluded.
if(MSVC)
  add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:/W4>")
  add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:/WX>")
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
else()
  add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:-Wall>")
  add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:-Wextra>")
  add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:-Wpedantic>")
  add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:-Werror>")
  add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:-Wshadow>")
  add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:-Wconversion>")
  add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:-Wsign-conversion>")
  add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:-Wold-style-cast>")
  add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:-Wdouble-promotion>")
endif()

list(APPEND DF_INSTALL_TARGETS dependency_fabric)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(DF_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(DF_BUILD_BENCHMARKS)
  add_subdirectory(benchmarks)
endif()

if(DF_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

if(DF_BUILD_TOOLS)
  add_subdirectory(tools)
endif()

if(DF_BUILD_CUDA)
  add_subdirectory(cuda)
endif()

# ---------------------------------------------------------------------------
# Install + CMake package export
# ---------------------------------------------------------------------------
set(DF_CMAKE_INSTALL_DIR "${CMAKE_INSTALL_LIBDIR}/cmake/DependencyFabric")

install(TARGETS ${DF_INSTALL_TARGETS}
        EXPORT DependencyFabricTargets
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(DIRECTORY include/dependency_fabric
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
        FILES_MATCHING PATTERN "*.hpp")

install(EXPORT DependencyFabricTargets
        FILE DependencyFabricTargets.cmake
        NAMESPACE DependencyFabric::
        DESTINATION ${DF_CMAKE_INSTALL_DIR})

configure_package_config_file(
    cmake/DependencyFabricConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/DependencyFabricConfig.cmake"
    INSTALL_DESTINATION ${DF_CMAKE_INSTALL_DIR})

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/DependencyFabricConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)

install(FILES
        "${CMAKE_CURRENT_BINARY_DIR}/DependencyFabricConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/DependencyFabricConfigVersion.cmake"
        DESTINATION ${DF_CMAKE_INSTALL_DIR})

install(FILES LICENSE README.md DESTINATION ${CMAKE_INSTALL_DATADIR}/dependency_fabric)