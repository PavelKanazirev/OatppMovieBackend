#[[
  MovieBackend - top level build script.

  Design notes for the reader:
    * Everything is C++23. We deliberately keep the CMake plain and explicit
      rather than clever: one static library holding the whole backend
      ("moviebackend_core"), one thin executable that starts the REST server,
      and one test executable.
    * Third-party dependencies are resolved in cmake/Dependencies.cmake.
      Anything not found on the system is downloaded with FetchContent, so a
      fresh clone builds on Windows 11 and WSL/Ubuntu with the same commands.
]]
cmake_minimum_required(VERSION 3.22)

project(MovieBackend
        VERSION 1.0.0
        DESCRIPTION "In-memory movie ticket booking backend with a REST API"
        LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Language level. C++23 is a hard requirement of the exercise.
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Emit build/compile_commands.json so that clangd, VS Code and clang-tidy
# resolve our include paths the same way the compiler does.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Put every artifact in one predictable place. The smoke tests and the
# Valgrind target rely on this location.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

# Single-config generators (Makefiles/Ninja) have no default build type.
# Without this, "no build type" means "no optimisation and no NDEBUG", which
# would silently give us a debug-flavoured build when the user asked for none.
get_property(_mb_is_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(NOT _mb_is_multi_config AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug" CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
                 Debug Release RelWithDebInfo MinSizeRel)
    message(STATUS "MovieBackend: CMAKE_BUILD_TYPE was empty, defaulting to Debug")
endif()

# ---------------------------------------------------------------------------
# User-facing options
# ---------------------------------------------------------------------------
option(MOVIEBACKEND_BUILD_TESTS "Build the GoogleTest unit tests" ON)
option(MOVIEBACKEND_BUILD_DOCS  "Add the Doxygen 'docs' target if Doxygen is present" ON)
option(MOVIEBACKEND_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(CTest)          # defines BUILD_TESTING and the "test" target
include(Dependencies)   # oatpp, spdlog, Boost, GoogleTest
include(CompilerWarnings)

# ---------------------------------------------------------------------------
# moviebackend_core - all of the actual backend logic.
#
# The REST layer, the domain model, the persistence layer and the services all
# live here so that the unit tests can link against exactly the same code the
# server runs.
# ---------------------------------------------------------------------------
add_library(moviebackend_core STATIC
    src/core/Errors.cpp
    src/core/Config.cpp
    src/core/Catalog.cpp
    src/core/Schedule.cpp
    src/core/SeatMap.cpp
    src/core/IdGenerator.cpp
    src/logging/Logging.cpp
    src/persistence/JsonCatalogStore.cpp
    src/persistence/PersistenceWorker.cpp
    src/service/BookingService.cpp
    src/service/AdminService.cpp
    src/rest/ErrorHandler.cpp
    src/rest/RestServer.cpp
)

target_include_directories(moviebackend_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(moviebackend_core
    PUBLIC
        oatpp::oatpp
        spdlog::spdlog
        Boost::json
        Threads::Threads
)

moviebackend_set_warnings(moviebackend_core)

# Compile-time log level. spdlog compiles out anything below
# SPDLOG_ACTIVE_LEVEL, so release builds carry no trace/debug call sites at
# all - see include/moviebackend/Logging.hpp for the full explanation.
target_compile_definitions(moviebackend_core PUBLIC
    $<IF:$<CONFIG:Debug>,SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_TRACE,SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_ERROR>
)

# ---------------------------------------------------------------------------
# The server executable
# ---------------------------------------------------------------------------
add_executable(moviebackend src/main.cpp)
target_link_libraries(moviebackend PRIVATE moviebackend_core)
moviebackend_set_warnings(moviebackend)

# Copy the sample configuration next to the binary so that the server can be
# started from the build tree with no arguments at all.
add_custom_command(TARGET moviebackend POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/config"
            "$<TARGET_FILE_DIR:moviebackend>/config"
    COMMENT "Copying config/ next to the moviebackend binary")

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(MOVIEBACKEND_BUILD_TESTS AND BUILD_TESTING)
    add_subdirectory(tests)
endif()

# ---------------------------------------------------------------------------
# Documentation and QA helper targets
# ---------------------------------------------------------------------------
include(Doxygen)
include(Valgrind)
include(PlantUML)

# ---------------------------------------------------------------------------
# Installation / packaging
# ---------------------------------------------------------------------------
include(GNUInstallDirs)

install(TARGETS moviebackend
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY include/moviebackend
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(DIRECTORY config/
        DESTINATION ${CMAKE_INSTALL_SYSCONFDIR}/moviebackend)

set(CPACK_PACKAGE_NAME "moviebackend")
set(CPACK_PACKAGE_VENDOR "MovieBackend study project")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${PROJECT_DESCRIPTION}")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_RESOURCE_FILE_README "${CMAKE_CURRENT_SOURCE_DIR}/README.md")
if(WIN32)
    set(CPACK_GENERATOR "ZIP")
else()
    set(CPACK_GENERATOR "TGZ")
endif()
include(CPack)

message(STATUS "----------------------------------------------------------")
message(STATUS "MovieBackend ${PROJECT_VERSION}")
message(STATUS "  build type ......... ${CMAKE_BUILD_TYPE}")
message(STATUS "  C++ standard ....... ${CMAKE_CXX_STANDARD}")
message(STATUS "  unit tests ......... ${MOVIEBACKEND_BUILD_TESTS}")
message(STATUS "----------------------------------------------------------")
