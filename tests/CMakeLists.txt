#[[
  Unit tests.

  ## Why GoogleTest

  Compared against Catch2 and doctest:
    * doctest compiles fastest but its ecosystem (death tests, parameterised
      fixtures, the mocking story) is the thinnest.
    * Catch2 v3 is pleasant to write and is a compiled library now, so its
      old "single header" advantage is gone.
    * **GoogleTest** wins here on two concrete points: it is packaged by every
      distro we target (so a WSL/Ubuntu build needs no download at all), and
      GoogleMock ships with it, which matters because the persistence tests
      substitute a fake CatalogStore. It is also the framework most likely to
      already be familiar in an automotive/embedded C++ shop.

  Every test binary is registered with CTest, so `ctest` runs the lot.
]]

add_executable(moviebackend_tests
    unit/test_main.cpp
    unit/SeatMapTest.cpp
    unit/ScheduleTest.cpp
    unit/ConfigTest.cpp
    unit/IdGeneratorTest.cpp
    unit/CatalogTest.cpp
    unit/CatalogConcurrencyTest.cpp
    unit/JsonCatalogStoreTest.cpp
    unit/PersistenceWorkerTest.cpp
    unit/BookingServiceTest.cpp
    unit/AdminServiceTest.cpp
)

target_link_libraries(moviebackend_tests
    PRIVATE
        moviebackend_core
        GTest::gtest
        Threads::Threads
)

target_include_directories(moviebackend_tests
    PRIVATE
        ${CMAKE_SOURCE_DIR}/src
)

moviebackend_set_warnings(moviebackend_tests)

include(GoogleTest)

# gtest_discover_tests registers each TEST() with CTest individually, so a
# failure report names the case rather than just "the test binary failed".
# DISCOVERY_MODE PRE_TEST defers enumeration to test time, which keeps
# cross-compiling and sanitiser builds working.
gtest_discover_tests(moviebackend_tests
    DISCOVERY_MODE PRE_TEST
    PROPERTIES
        TIMEOUT 120
)

# ---------------------------------------------------------------------------
# Python smoke tests - the end-to-end IPC path.
#
# These start the real server binary and talk to it over real HTTP, which is
# the only way to cover the request parsing, routing, DTO mapping and error
# translation that the C++ unit tests deliberately skip.
# ---------------------------------------------------------------------------
find_package(Python3 COMPONENTS Interpreter QUIET)

if(Python3_Interpreter_FOUND)
    add_test(NAME smoke_tests
             COMMAND ${Python3_EXECUTABLE}
                     "${CMAKE_CURRENT_SOURCE_DIR}/smoke/run_smoke_tests.py"
                     --binary "$<TARGET_FILE:moviebackend>"
                     --source-dir "${CMAKE_SOURCE_DIR}")

    set_tests_properties(smoke_tests PROPERTIES
        TIMEOUT 180
        LABELS "smoke")

    message(STATUS "MovieBackend: smoke tests registered (${Python3_EXECUTABLE})")
else()
    message(STATUS "MovieBackend: Python3 not found - smoke tests not registered")
endif()
