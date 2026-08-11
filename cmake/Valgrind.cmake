#[[
  Valgrind / memcheck QA targets.  Linux and WSL only.

  How this fits a cross-platform project:
    Valgrind does not exist on Windows.  Rather than pretend otherwise, the
    memcheck targets are simply not defined on Windows - the Windows build is
    still complete and testable, it just uses the compiler sanitisers or
    Application Verifier instead.  In CI the memcheck node runs on the
    Linux/WSL agent only.

  Two targets are provided because they answer two different questions:

    memcheck-tests   Does the *logic* leak or touch uninitialised memory?
                     Runs the GoogleTest binary under memcheck.  Fully
                     deterministic and safe for CI gating.

    memcheck-server  Does the *server* leak across a real request cycle?
                     Starts the real backend under memcheck, drives it with
                     the Python smoke tests, then shuts it down cleanly via
                     SIGINT so that destructors actually run.  This is the one
                     that exercises the REST/IPC boundary.
]]

if(WIN32)
    message(STATUS "MovieBackend: Valgrind targets skipped (Windows host)")
    return()
endif()

find_program(MOVIEBACKEND_VALGRIND_EXECUTABLE valgrind)

if(NOT MOVIEBACKEND_VALGRIND_EXECUTABLE)
    message(STATUS "MovieBackend: valgrind not found - memcheck targets unavailable")
    return()
endif()

set(MOVIEBACKEND_VALGRIND_FLAGS
    --tool=memcheck
    --leak-check=full
    --show-leak-kinds=definite,indirect
    --track-origins=yes
    --errors-for-leak-kinds=definite
    --error-exitcode=99
    "--suppressions=${CMAKE_CURRENT_SOURCE_DIR}/tests/valgrind.supp")

if(MOVIEBACKEND_BUILD_TESTS AND BUILD_TESTING)
    add_custom_target(memcheck-tests
        COMMAND ${MOVIEBACKEND_VALGRIND_EXECUTABLE} ${MOVIEBACKEND_VALGRIND_FLAGS}
                $<TARGET_FILE:moviebackend_tests>
        DEPENDS moviebackend_tests
        COMMENT "Running unit tests under Valgrind memcheck"
        VERBATIM)
endif()

# The server run needs a driver script because it has to start the process,
# wait for the port, run the smoke tests and then signal a clean shutdown.
add_custom_target(memcheck-server
    COMMAND ${CMAKE_COMMAND} -E env
            "MOVIEBACKEND_BINARY=$<TARGET_FILE:moviebackend>"
            "MOVIEBACKEND_VALGRIND=${MOVIEBACKEND_VALGRIND_EXECUTABLE}"
            "MOVIEBACKEND_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
            "MOVIEBACKEND_BUILD_DIR=${CMAKE_BINARY_DIR}"
            bash "${CMAKE_CURRENT_SOURCE_DIR}/scripts/run_memcheck_server.sh"
    DEPENDS moviebackend
    COMMENT "Running the backend under Valgrind memcheck while the smoke tests drive it"
    VERBATIM)

message(STATUS "MovieBackend: memcheck-tests / memcheck-server targets available")
