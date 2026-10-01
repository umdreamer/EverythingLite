cmake_minimum_required(VERSION 3.21)
project(EverythingLite VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(BUILD_GUI "Build the Qt desktop GUI" ON)
option(BUILD_TESTS "Build tests" ON)

find_package(Threads REQUIRED)
find_package(SQLite3 REQUIRED)

add_library(everything_core
    src/core/path_utils.cpp
    src/core/scanner.cpp
    src/core/database.cpp
    src/core/search_engine.cpp
    src/core/index_manager.cpp
    src/platform/watcher_factory.cpp
)

target_include_directories(everything_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(everything_core PUBLIC SQLite::SQLite3 Threads::Threads)
target_compile_definitions(everything_core PUBLIC EVERYTHING_LITE_VERSION="${PROJECT_VERSION}")

if(APPLE)
    target_sources(everything_core PRIVATE src/platform/mac_fsevents_watcher.cpp)
    find_library(CORESERVICES_FRAMEWORK CoreServices REQUIRED)
    target_link_libraries(everything_core PUBLIC ${CORESERVICES_FRAMEWORK})
endif()

add_executable(everything-lite-cli src/cli/main.cpp)
target_link_libraries(everything-lite-cli PRIVATE everything_core)

if(BUILD_GUI)
    find_package(Qt6 QUIET COMPONENTS Widgets)
    if(Qt6_FOUND)
        set(CMAKE_AUTOMOC ON)
        add_executable(everything-lite MACOSX_BUNDLE
            src/ui/main.cpp
            src/ui/main_window.cpp
            src/ui/main_window.h
            src/ui/search_result_model.cpp
            src/ui/search_result_model.h
            src/ui/root_settings_dialog.cpp
            src/ui/root_settings_dialog.h
        )
        target_link_libraries(everything-lite PRIVATE everything_core Qt6::Widgets)
        set_target_properties(everything-lite PROPERTIES
            MACOSX_BUNDLE_BUNDLE_NAME "Everything Lite"
            MACOSX_BUNDLE_GUI_IDENTIFIER "org.cichi.everything-lite"
            MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
            MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
        )
    else()
        message(WARNING "Qt6 Widgets not found: GUI target skipped. Install Qt6 and reconfigure to build everything-lite.")
    endif()
endif()

if(BUILD_TESTS)
    enable_testing()
    add_executable(test_core tests/test_core.cpp)
    target_link_libraries(test_core PRIVATE everything_core)
    add_test(NAME core COMMAND test_core)
endif()

install(TARGETS everything-lite-cli RUNTIME DESTINATION bin)
if(TARGET everything-lite)
    install(TARGETS everything-lite BUNDLE DESTINATION . RUNTIME DESTINATION bin)
endif()
