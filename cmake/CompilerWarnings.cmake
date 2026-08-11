#[[
  A single helper that applies our warning set to a target.

  Kept as a function (rather than global CMAKE_CXX_FLAGS) so that the
  third-party targets pulled in by FetchContent are not affected - they have
  their own idea of what is warning-clean.
]]

function(moviebackend_set_warnings target)

    set(gcc_like_warnings
        -Wall
        -Wextra
        -Wpedantic
        -Wshadow                # a classic source of subtle bugs
        -Wnon-virtual-dtor
        -Wold-style-cast
        -Wcast-align
        -Wunused
        -Woverloaded-virtual
        -Wconversion
        -Wsign-conversion
        -Wdouble-promotion
        -Wformat=2
    )

    set(msvc_warnings
        /W4
        /permissive-            # standards conformance
        /w14640                 # thread-unsafe static member initialisation
        /w14826                 # conversion is sign-extended
        /Zc:__cplusplus         # report the real __cplusplus value
    )

    if(MOVIEBACKEND_WARNINGS_AS_ERRORS)
        list(APPEND gcc_like_warnings -Werror)
        list(APPEND msvc_warnings /WX)
    endif()

    target_compile_options(${target} PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:${gcc_like_warnings}>
        $<$<CXX_COMPILER_ID:MSVC>:${msvc_warnings}>
    )

    if(MSVC)
        # Without this, MSVC macro expansion breaks oatpp's codegen macros.
        target_compile_options(${target} PRIVATE /Zc:preprocessor)
        # Silence "unsafe" CRT warnings triggered by third-party headers.
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()

endfunction()
