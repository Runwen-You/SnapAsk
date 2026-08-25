function(snapask_set_project_warnings target_name)
    if(NOT TARGET "${target_name}")
        message(FATAL_ERROR "snapask_set_project_warnings: unknown target '${target_name}'")
    endif()

    target_compile_options("${target_name}" PRIVATE
        /W4
        /permissive-
        /Zc:__cplusplus
        /Zc:preprocessor
        /utf-8
    )

    if(SNAPASK_WARNINGS_AS_ERRORS)
        target_compile_options("${target_name}" PRIVATE /WX)
    endif()
endfunction()

