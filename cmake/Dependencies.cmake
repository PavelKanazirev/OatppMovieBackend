#[[
  Third-party dependency resolution.

  Policy: try the system package first (fast, and what a distro user expects),
  fall back to FetchContent so that a bare clone on Windows still builds with
  no manual dependency installation.

  Dependencies and why each one is here:
    * oatpp   - the REST framework, mandated by the exercise.
    * spdlog  - logging. See docs/decisions in README.md for the comparison
                against glog and Boost.Log.
    * Boost   - used for JSON serialisation/deserialisation ONLY (Boost.JSON).
                This is the deliberate, single-purpose Boost dependency.
    * GTest   - unit test framework.
]]

include(FetchContent)

# Keep every download in one directory inside the build tree so that a
# reconfigure does not re-download from the network.
set(FETCHCONTENT_BASE_DIR "${CMAKE_BINARY_DIR}/_deps" CACHE PATH "" FORCE)

find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# Boost.JSON
#
# Boost is restricted to serialisation/deserialisation in this project.
# Boost.JSON gives us a proper DOM with real value types (int64/double/bool/
# string), which matters because the admin REST API rewrites the on-disk
# catalog at runtime and we must not degrade every number to a string on the
# round trip.
#
# Boost.JSON can be used header-only by defining BOOST_JSON_STANDALONE-style
# header inclusion, but the compiled library is what distros ship, so we
# prefer the compiled target and fall back to fetching Boost only if needed.
# ---------------------------------------------------------------------------
find_package(Boost CONFIG QUIET COMPONENTS json)

if(TARGET Boost::json)
    message(STATUS "MovieBackend: using system Boost.JSON")
else()
    message(STATUS "MovieBackend: fetching Boost (including JSON dependencies)")

    set(BOOST_INCLUDE_LIBRARIES json)
    set(BOOST_ENABLE_CMAKE ON)

    FetchContent_Declare(
        boost
        GIT_REPOSITORY https://github.com/boostorg/boost.git
        GIT_TAG boost-1.87.0 # Or a pinned newer Boost release you choose
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(boost)
endif()

# ---------------------------------------------------------------------------
# spdlog
# ---------------------------------------------------------------------------
find_package(spdlog 1.10 QUIET)

if(spdlog_FOUND)
    message(STATUS "MovieBackend: using system spdlog ${spdlog_VERSION}")
else()
    message(STATUS "MovieBackend: fetching spdlog")
    set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
    set(SPDLOG_BUILD_TESTS   OFF CACHE BOOL "" FORCE)
    set(SPDLOG_INSTALL       OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(spdlog
        GIT_REPOSITORY https://github.com/gabime/spdlog.git
        GIT_TAG        v1.14.1
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(spdlog)
endif()

# ---------------------------------------------------------------------------
# oatpp
#
# oatpp does not ship in distro repositories, so it is always fetched.
# 1.3.1 is the newest tagged release at the time of writing.
# ---------------------------------------------------------------------------
find_package(oatpp 1.3.0 QUIET)

if(oatpp_FOUND)
    message(STATUS "MovieBackend: using system oatpp")
else()
    message(STATUS "MovieBackend: fetching oatpp 1.3.1")
    set(OATPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    # oatpp installs its own CMake package files; we only want the library.
    set(OATPP_INSTALL OFF CACHE BOOL "" FORCE)
    # On MSVC oatpp defaults to /MT while everything else here uses /MD.
    set(OATPP_MSVC_LINK_STATIC_RUNTIME OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(oatpp
        GIT_REPOSITORY https://github.com/oatpp/oatpp.git
        GIT_TAG        1.3.1
        GIT_SHALLOW    TRUE
    )
	set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
    FetchContent_MakeAvailable(oatpp)
	unset(CMAKE_POLICY_VERSION_MINIMUM)

    # The in-tree build exports the plain target name "oatpp"; the installed
    # package exports "oatpp::oatpp". Normalise on the namespaced name.
    if(TARGET oatpp AND NOT TARGET oatpp::oatpp)
        add_library(oatpp::oatpp ALIAS oatpp)
    endif()

    # oatpp 1.3.1 predates C++20 and emits a lot of noise when compiled as
    # C++23. It is a third-party dependency, so silence it rather than patch it.
    if(TARGET oatpp)
        target_compile_options(oatpp PRIVATE
            $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-w>
            $<$<CXX_COMPILER_ID:MSVC>:/w>)
        target_include_directories(oatpp SYSTEM INTERFACE
            $<BUILD_INTERFACE:${oatpp_SOURCE_DIR}/src>)
    endif()
endif()

# ---------------------------------------------------------------------------
# GoogleTest (only needed when tests are enabled)
# ---------------------------------------------------------------------------
if(MOVIEBACKEND_BUILD_TESTS)
    find_package(GTest 1.11 QUIET)

    if(GTest_FOUND)
        message(STATUS "MovieBackend: using system GoogleTest ${GTest_VERSION}")
    else()
        message(STATUS "MovieBackend: fetching GoogleTest")
        # Required on Windows so that gtest links against the same CRT as us.
        set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
        set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
        FetchContent_Declare(googletest
            GIT_REPOSITORY https://github.com/google/googletest.git
            GIT_TAG        v1.14.0
            GIT_SHALLOW    TRUE
        )
        FetchContent_MakeAvailable(googletest)
    endif()
endif()
