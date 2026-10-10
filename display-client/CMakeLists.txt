cmake_minimum_required(VERSION 3.21)

project(
    signage-display-client
    VERSION 0.1.0
    LANGUAGES CXX
)

option(
    SIGNAGE_BUILD_APP
    "Build the Qt Quick/Multimedia display executable"
    ON
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(
    Qt6 6.4
    REQUIRED
    COMPONENTS
        Core
        Gui
        Test
)

if(SIGNAGE_BUILD_APP)
    find_package(
        Qt6 6.4
        REQUIRED
        COMPONENTS
            Multimedia
            Qml
            Quick
    )
endif()

qt_standard_project_setup(REQUIRES 6.4)

qt_add_library(signage_display_core STATIC
    src/active_bundle_watcher.cpp
    src/active_bundle_watcher.h
    src/bundle_parser.cpp
    src/bundle_parser.h
    src/bundle_types.h
    src/clock_formatter.cpp
    src/clock_formatter.h
    src/display_controller.cpp
    src/display_controller.h
    src/layout_geometry.cpp
    src/layout_geometry.h
    src/playlist_navigator.cpp
    src/playlist_navigator.h
    src/schedule_projection.cpp
    src/schedule_projection.h
    src/screen_selector.cpp
    src/screen_selector.h
)

target_include_directories(signage_display_core PUBLIC src)
target_link_libraries(
    signage_display_core
    PUBLIC
        Qt6::Core
        Qt6::Gui
)

target_compile_options(
    signage_display_core
    PRIVATE
        $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic>
)

if(SIGNAGE_BUILD_APP)
    set_source_files_properties(
        qml/Main.qml
        PROPERTIES
            QT_RESOURCE_ALIAS Main.qml
    )
    set_source_files_properties(
        assets/Desktop.svg
        PROPERTIES
            QT_RESOURCE_ALIAS Desktop.svg
    )

    qt_add_executable(signage-display-client
        src/main.cpp
        src/screen_coordinator.cpp
        src/screen_coordinator.h
    )

    qt_add_qml_module(
        signage-display-client
        URI Signage.Display
        VERSION 1.0
        RESOURCE_PREFIX /qt/qml
        QML_FILES
            qml/Main.qml
    )

    qt_add_resources(
        signage-display-client
        embedded_layout
        PREFIX
            /assets
        FILES
            assets/Desktop.svg
    )

    target_link_libraries(
        signage-display-client
        PRIVATE
            signage_display_core
            Qt6::Multimedia
            Qt6::Qml
            Qt6::Quick
    )
endif()

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
