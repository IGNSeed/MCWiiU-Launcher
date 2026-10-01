# Saucer enumerates assets at configure time. Frontend changes are configure
# dependencies, so rebuilding also refreshes the complete embedding file list.
function(mcwiiu_build_frontend source_dir staging_dir)
    mcwiiu_require_external_path("${staging_dir}")
    find_program(MCWIIU_NPM_EXECUTABLE NAMES npm.cmd REQUIRED)
    find_program(MCWIIU_NODE_EXECUTABLE NAMES node REQUIRED)
    execute_process(COMMAND "${MCWIIU_NODE_EXECUTABLE}" --version
        OUTPUT_VARIABLE node_version OUTPUT_STRIP_TRAILING_WHITESPACE
        COMMAND_ERROR_IS_FATAL ANY)

    file(GLOB_RECURSE source_files CONFIGURE_DEPENDS RELATIVE "${source_dir}" "${source_dir}/src/*")
    list(APPEND source_files package.json package-lock.json index.html tsconfig.json vite.config.ts)
    file(GLOB_RECURSE staged_sources RELATIVE "${staging_dir}" "${staging_dir}/src/*")
    foreach(staged_source IN LISTS staged_sources)
        if (NOT staged_source IN_LIST source_files)
            mcwiiu_require_external_path("${staging_dir}/${staged_source}")
            file(REMOVE "${staging_dir}/${staged_source}")
        endif()
    endforeach()
    set(input_fingerprint "${node_version}")
    foreach(input IN LISTS source_files)
        configure_file("${source_dir}/${input}" "${staging_dir}/${input}" COPYONLY)
        file(SHA256 "${source_dir}/${input}" input_hash)
        string(APPEND input_fingerprint "\n${input}:${input_hash}")
    endforeach()
    string(SHA256 input_fingerprint "${input_fingerprint}")
    file(SHA256 "${source_dir}/package-lock.json" dependency_fingerprint)
    set(previous_dependencies "")
    if (EXISTS "${staging_dir}/dependencies.sha256")
        file(READ "${staging_dir}/dependencies.sha256" previous_dependencies)
    endif()
    if (NOT previous_dependencies STREQUAL dependency_fingerprint OR
        NOT EXISTS "${staging_dir}/node_modules/.package-lock.json")
        execute_process(COMMAND "${MCWIIU_NPM_EXECUTABLE}" ci --no-audit --no-fund
                --cache "${CMAKE_BINARY_DIR}/npm-cache"
            WORKING_DIRECTORY "${staging_dir}" COMMAND_ERROR_IS_FATAL ANY)
        file(WRITE "${staging_dir}/dependencies.sha256" "${dependency_fingerprint}")
    endif()

    set(previous_inputs "")
    if (EXISTS "${staging_dir}/inputs.sha256")
        file(READ "${staging_dir}/inputs.sha256" previous_inputs)
    endif()
    if (NOT previous_inputs STREQUAL input_fingerprint OR NOT EXISTS "${staging_dir}/dist/index.html")
        execute_process(COMMAND "${MCWIIU_NPM_EXECUTABLE}" run build
            WORKING_DIRECTORY "${staging_dir}" COMMAND_ERROR_IS_FATAL ANY)
        file(WRITE "${staging_dir}/inputs.sha256" "${input_fingerprint}")
    endif()
endfunction()
