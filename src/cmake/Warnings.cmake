# Warnings-as-errors for our own targets only. Anything vendored or fetched is
# pinned upstream code and its diagnostics are not ours to fix.

function(zr_set_warnings target)
    # Standard C11 without compiler extensions.
    set_target_properties(${target} PROPERTIES C_EXTENSIONS OFF)
    if(MSVC)
        target_compile_options(${target} PRIVATE
            /W4 /WX
            /w44242  # conversion, possible loss of data
            /w44254
            /w44287 /w44289
            /w44296  # expression is always true/false
            /w44826  # sign-extended conversion
            /w44905 /w44906
            /utf-8
        )
        # Fixed-width hardware arithmetic narrows deliberately and everywhere,
        # so the blanket narrowing warnings are noise.
        target_compile_options(${target} PRIVATE /wd4244 /wd4267)
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
    else()
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Werror -Wshadow -Wformat=2 -Wno-unused-parameter
            # The tables leave their last fields to C's zero fill, and every message is formatted into a buffer of its own
            # size with snprintf, which cuts it by design.
            -Wno-missing-field-initializers -Wno-format-truncation
        )
    endif()
endfunction()
