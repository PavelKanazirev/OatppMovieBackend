#[[
  Doc-as-code: generates browsable HTML API documentation from the comments in
  include/moviebackend/*.hpp.

  The target is optional - if Doxygen is not installed the rest of the build is
  unaffected and we just print a note. Build it with:

      cmake --build build --target docs
]]

if(NOT MOVIEBACKEND_BUILD_DOCS)
    return()
endif()

find_package(Doxygen QUIET)

if(NOT DOXYGEN_FOUND)
    message(STATUS "MovieBackend: Doxygen not found - the 'docs' target is unavailable")
    return()
endif()

set(MOVIEBACKEND_DOXYGEN_INPUT
    "${CMAKE_CURRENT_SOURCE_DIR}/include \\\n                         ${CMAKE_CURRENT_SOURCE_DIR}/src \\\n                         ${CMAKE_CURRENT_SOURCE_DIR}/docs/api-overview.md")
set(MOVIEBACKEND_DOXYGEN_OUTPUT "${CMAKE_BINARY_DIR}/docs")

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/docs/Doxyfile.in"
    "${CMAKE_BINARY_DIR}/Doxyfile"
    @ONLY)

add_custom_target(docs
    COMMAND ${DOXYGEN_EXECUTABLE} "${CMAKE_BINARY_DIR}/Doxyfile"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMENT "Generating API documentation with Doxygen -> ${MOVIEBACKEND_DOXYGEN_OUTPUT}/html/index.html"
    VERBATIM)

message(STATUS "MovieBackend: 'docs' target available (Doxygen ${DOXYGEN_VERSION})")
