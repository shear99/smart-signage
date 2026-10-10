qt_add_executable(test-display-core
    test_display_core.cpp
)

qt_add_executable(check-display-bundle
    check_bundle.cpp
)

target_link_libraries(
    check-display-bundle
    PRIVATE
        signage_display_core
)

target_link_libraries(
    test-display-core
    PRIVATE
        signage_display_core
        Qt6::Test
)

target_compile_definitions(
    test-display-core
    PRIVATE
        DISPLAY_CLIENT_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
)

add_test(NAME display-core COMMAND test-display-core)
set_tests_properties(
    display-core
    PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

if(TARGET signage-display-client)
    add_test(
        NAME display-qml-smoke
        COMMAND
            signage-display-client
            --smoke-test
            --content-root
            "${PROJECT_SOURCE_DIR}/examples/content-root"
            --drm-root
            "${CMAKE_CURRENT_BINARY_DIR}/missing-drm"
    )
    set_tests_properties(
        display-qml-smoke
        PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )
endif()

find_program(UV_EXECUTABLE uv)
if(UV_EXECUTABLE)
    add_test(
        NAME server-compiler-contract
        COMMAND
            ${UV_EXECUTABLE}
            run
            python
            ${CMAKE_CURRENT_SOURCE_DIR}/server_compiler_contract.py
            --checker
            $<TARGET_FILE:check-display-bundle>
    )
    set_tests_properties(
        server-compiler-contract
        PROPERTIES
            WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}/.."
    )
endif()
