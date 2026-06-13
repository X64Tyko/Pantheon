project(Hades)

find_program(NPM pnpm)

if(NOT NPM)
    message(STATUS "[hades] pnpm not found — UI build target disabled")
    # Define an empty target so the parent can still call add_dependencies(kairos hades)
    add_custom_target(hades)
    return()
endif()

message(STATUS "[hades] pnpm found: ${NPM}")

# Stamp file tracks whether node_modules is current — avoids pnpm install on every build.
add_custom_command(
    OUTPUT  ${CMAKE_CURRENT_BINARY_DIR}/pnpm_install.stamp
    COMMAND ${NPM} install
    COMMAND ${CMAKE_COMMAND} -E touch ${CMAKE_CURRENT_BINARY_DIR}/pnpm_install.stamp
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/package.json
    COMMENT "[hades] pnpm install"
)

# Always run vite build; stamp ensures install ran first.
add_custom_target(hades ALL
    COMMAND ${NPM} run build
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/pnpm_install.stamp
    COMMENT "[hades] vite build → kairos/ui-dist"
)
