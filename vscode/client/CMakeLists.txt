set(iv_client_dist "${CMAKE_CURRENT_BINARY_DIR}/dist")
set(iv_client_stamp "${CMAKE_CURRENT_BINARY_DIR}/client-built")
file(GLOB_RECURSE iv_client_sources LIST_DIRECTORIES false CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*"
    "${CMAKE_CURRENT_SOURCE_DIR}/webview/*"
    "${CMAKE_CURRENT_SOURCE_DIR}/bindings/*.json")
file(GLOB iv_client_installed_lock CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/node_modules/.package-lock.json")
set(iv_client_inputs "${CMAKE_CURRENT_BINARY_DIR}/client-inputs.txt")
file(GENERATE OUTPUT "${iv_client_inputs}"
    CONTENT "${iv_client_sources}\n${iv_client_installed_lock}\n")

# The launcher builds this target explicitly. Native builds and tests do not
# need npm or the extension's dependencies.
add_custom_command(
    OUTPUT "${iv_client_stamp}"
    BYPRODUCTS
        "${iv_client_dist}/extension.js"
        "${iv_client_dist}/extension.js.map"
        "${iv_client_dist}/headlessClient.js"
        "${iv_client_dist}/headlessClient.js.map"
        "${iv_client_dist}/wasmPresentationNodeEntry.js"
        "${iv_client_dist}/wasmPresentationNodeEntry.js.map"
        "${iv_client_dist}/webview/main.css"
        "${iv_client_dist}/webview/main.js"
        "${iv_client_dist}/webview/main.js.map"
    COMMAND "${CMAKE_COMMAND}" -E env "IV_CLIENT_OUT_DIR=${iv_client_dist}"
        npm --prefix "${CMAKE_CURRENT_SOURCE_DIR}" run build
    COMMAND "${CMAKE_COMMAND}" -E touch "${iv_client_stamp}"
    DEPENDS
        "${iv_client_inputs}"
        ${iv_client_sources}
        ${iv_client_installed_lock}
        "${CMAKE_CURRENT_SOURCE_DIR}/package.json"
        "${CMAKE_CURRENT_SOURCE_DIR}/package-lock.json"
        "${CMAKE_CURRENT_SOURCE_DIR}/esbuild.mjs"
        "${CMAKE_CURRENT_SOURCE_DIR}/vite.webview.config.mts"
        "${CMAKE_CURRENT_SOURCE_DIR}/tsconfig.json"
    COMMENT "Building the Intravenous VS Code client"
    VERBATIM)
add_custom_target(intravenous_vscode_client DEPENDS "${iv_client_stamp}")
