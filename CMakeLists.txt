cmake_minimum_required(VERSION 3.21.1)
project(EverythingLite VERSION 0.4.12 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(BUILD_GUI "Build the Qt desktop GUI" ON)
option(BUILD_TESTS "Build tests" ON)
option(REQUIRE_GUI "Fail configuration if the requested GUI cannot be built" OFF)

if(REQUIRE_GUI AND NOT BUILD_GUI)
    message(FATAL_ERROR "REQUIRE_GUI needs BUILD_GUI=ON")
endif()

# Avoid embedding the developer checkout path in diagnostics and debug symbols.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
    add_compile_options("-ffile-prefix-map=${CMAKE_SOURCE_DIR}=.")
endif()

find_package(Threads REQUIRED)
find_package(SQLite3 REQUIRED)

# CMake 4.x renamed the imported SQLite target to SQLite3::SQLite3.
# Keep a fallback for older CMake versions so the project remains portable.
if(TARGET SQLite3::SQLite3)
    set(EVERYTHING_LITE_SQLITE_TARGET SQLite3::SQLite3)
elseif(TARGET SQLite::SQLite3)
    set(EVERYTHING_LITE_SQLITE_TARGET SQLite::SQLite3)
else()
    message(FATAL_ERROR "No imported SQLite3 CMake target was found")
endif()

add_library(everything_core
    src/core/exclusion_policy.cpp
    src/core/app_paths.cpp
    src/core/path_utils.cpp
    src/core/directory_sync.cpp
    src/core/scanner.cpp
    src/core/database.cpp
    src/core/source_catalog.cpp
    src/core/source_probe.cpp
    src/core/source_verification.cpp
    src/core/source_scan.cpp
    src/core/scan_request.cpp
    src/core/search_engine.cpp
    src/core/search_service.cpp
    src/core/search_trace.cpp
    src/core/search_query.cpp
    src/core/index_manager.cpp
    src/platform/watcher_factory.cpp
)

target_include_directories(everything_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(everything_core PUBLIC ${EVERYTHING_LITE_SQLITE_TARGET} Threads::Threads)
target_compile_definitions(everything_core PUBLIC EVERYTHING_LITE_VERSION="${PROJECT_VERSION}")

if(APPLE)
    target_sources(everything_core PRIVATE src/platform/mac_fsevents_watcher.cpp)
    find_library(CORESERVICES_FRAMEWORK CoreServices REQUIRED)
    find_library(COREFOUNDATION_FRAMEWORK CoreFoundation REQUIRED)
    target_link_libraries(everything_core PUBLIC ${CORESERVICES_FRAMEWORK} ${COREFOUNDATION_FRAMEWORK})
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_sources(everything_core PRIVATE src/platform/linux_inotify_watcher.cpp)
endif()

add_executable(everything-lite-cli src/cli/main.cpp)
target_link_libraries(everything-lite-cli PRIVATE everything_core)

if(BUILD_GUI)
    find_package(Qt6 QUIET COMPONENTS Widgets)
    if(Qt6_FOUND)
        set(CMAKE_AUTOMOC ON)
        add_library(everything_ui STATIC
            src/ui/main_window.cpp
            src/ui/main_window_sync.cpp
            src/ui/result_presentation.h
            src/ui/main_window.h
            src/ui/desktop_actions.cpp
            src/ui/desktop_actions.h
            src/ui/file_preview.cpp
            src/ui/file_preview.h
            src/ui/search_result_model.cpp
            src/ui/search_result_model.h
            src/ui/search_worker.cpp
            src/ui/search_worker.h
            src/ui/root_settings_dialog.cpp
            src/ui/root_settings_dialog.h
        )
        target_link_libraries(everything_ui PUBLIC everything_core Qt6::Widgets)
        add_executable(everything-lite src/ui/main.cpp)
        target_link_libraries(everything-lite PRIVATE everything_ui)
        set_target_properties(everything-lite PROPERTIES OUTPUT_NAME EverythingLite)
        if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
            find_package(Qt6 REQUIRED COMPONENTS DBus)
            target_link_libraries(everything_ui PUBLIC Qt6::DBus)
        endif()

        if(APPLE)
            set_target_properties(everything-lite PROPERTIES
                MACOSX_BUNDLE TRUE
                MACOSX_BUNDLE_BUNDLE_NAME "Everything Lite"
                MACOSX_BUNDLE_GUI_IDENTIFIER "org.cichi.everything-lite"
                MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
                MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
            )
        endif()
    else()
        if(REQUIRE_GUI)
            message(FATAL_ERROR "Qt6 Widgets not found: requested GUI is unavailable")
        endif()
        message(WARNING "Qt6 Widgets not found: GUI target skipped. Install Qt6/qtbase and reconfigure to build EverythingLite.")
    endif()
endif()

if(BUILD_TESTS)
    enable_testing()
    if(TARGET everything_ui)
        find_package(Qt6 REQUIRED COMPONENTS Test)
        add_executable(test_desktop_actions tests/test_desktop_actions.cpp)
        target_link_libraries(test_desktop_actions PRIVATE everything_ui Qt6::Test)
        if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
            find_program(DBUS_RUN_SESSION dbus-run-session REQUIRED)
            add_test(NAME desktop_actions COMMAND ${DBUS_RUN_SESSION} -- $<TARGET_FILE:test_desktop_actions>)
        else()
            add_test(NAME desktop_actions COMMAND test_desktop_actions)
        endif()
        set_tests_properties(desktop_actions PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 30)
        add_executable(test_ui tests/test_ui.cpp)
        target_link_libraries(test_ui PRIVATE everything_ui Qt6::Test)
        target_compile_definitions(test_ui PRIVATE EVERYTHING_LITE_CLI_PATH="$<TARGET_FILE:everything-lite-cli>")
        add_dependencies(test_ui everything-lite-cli)
        add_test(NAME ui COMMAND test_ui)
        # Real debounce/backoff scenarios need headroom on slower CI/QEMU hosts.
        # This limits only the whole test process, not any product deadline.
        set_tests_properties(ui PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 180)
    endif()
    add_executable(test_exclusion_policy tests/test_exclusion_policy.cpp)
    target_link_libraries(test_exclusion_policy PRIVATE everything_core)
    add_test(NAME exclusion_policy COMMAND test_exclusion_policy)
    if(APPLE)
        add_executable(test_directory_sync tests/test_directory_sync.cpp)
        target_link_libraries(test_directory_sync PRIVATE everything_core)
        add_test(NAME directory_sync COMMAND test_directory_sync)
        set_tests_properties(directory_sync PROPERTIES TIMEOUT 30)
        add_executable(test_source_verification tests/test_source_verification.cpp)
        target_link_libraries(test_source_verification PRIVATE everything_core)
        add_test(NAME source_verification COMMAND test_source_verification)
        set_tests_properties(source_verification PROPERTIES TIMEOUT 30)
        add_executable(test_scan_request tests/test_scan_request.cpp)
        target_link_libraries(test_scan_request PRIVATE everything_core)
        add_test(NAME scan_request COMMAND test_scan_request)
        set_tests_properties(scan_request PROPERTIES TIMEOUT 30)
        add_executable(test_source_snapshot tests/test_source_snapshot.cpp)
        target_link_libraries(test_source_snapshot PRIVATE everything_core)
        add_test(NAME source_snapshot COMMAND test_source_snapshot)
        set_tests_properties(source_snapshot PROPERTIES TIMEOUT 60)
        add_executable(test_source_scan tests/test_source_scan.cpp)
        target_link_libraries(test_source_scan PRIVATE everything_core)
        add_test(NAME source_scan COMMAND test_source_scan)
        set_tests_properties(source_scan PROPERTIES TIMEOUT 60)
    endif()
    add_executable(test_source_probe tests/test_source_probe.cpp)
    target_link_libraries(test_source_probe PRIVATE everything_core)
    add_test(NAME source_probe COMMAND test_source_probe)
    set_tests_properties(source_probe PROPERTIES TIMEOUT 30)
    add_executable(test_source_catalog tests/test_source_catalog.cpp)
    target_link_libraries(test_source_catalog PRIVATE everything_core)
    add_test(NAME source_catalog COMMAND test_source_catalog)
    add_executable(test_database_performance tests/test_database_performance.cpp)
    target_link_libraries(test_database_performance PRIVATE everything_core)
    add_test(NAME database_performance COMMAND test_database_performance)
    add_test(NAME path_changes COMMAND test_database_performance path)
    add_executable(test_core tests/test_core.cpp)
    target_link_libraries(test_core PRIVATE everything_core)
    add_test(NAME core COMMAND test_core)
    find_package(Python3 QUIET COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        add_test(NAME cli_parity COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_SOURCE_DIR}/tests/check_cli_parity.py $<TARGET_FILE:everything-lite-cli>)
        set_tests_properties(cli_parity PROPERTIES TIMEOUT 60)
        if(APPLE)
            add_test(NAME cli_sources COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/check_cli_sources.py $<TARGET_FILE:everything-lite-cli>)
            set_tests_properties(cli_sources PROPERTIES TIMEOUT 60)
            add_test(NAME cli_scan COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/check_cli_scan.py $<TARGET_FILE:everything-lite-cli> $<TARGET_FILE:test_scan_request>)
            set_tests_properties(cli_scan PROPERTIES TIMEOUT 90)
        endif()
        add_test(NAME cli_excludes COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_SOURCE_DIR}/tests/check_cli_excludes.py $<TARGET_FILE:everything-lite-cli>)
        set_tests_properties(cli_excludes PROPERTIES TIMEOUT 60)
    endif()
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        add_executable(test_linux_watcher tests/test_linux_watcher.cpp)
        target_link_libraries(test_linux_watcher PRIVATE everything_core)
        add_test(NAME linux_watcher COMMAND test_linux_watcher)
        set_tests_properties(linux_watcher PROPERTIES TIMEOUT 60)
    endif()
endif()

install(TARGETS everything-lite-cli RUNTIME DESTINATION bin)
if(TARGET everything-lite)
    install(TARGETS everything-lite BUNDLE DESTINATION . RUNTIME DESTINATION bin)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND TARGET everything-lite)
    include(GNUInstallDirs)
    install(FILES packaging/linux/org.everythinglite.EverythingLite.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
    install(FILES assets/everything-lite.svg
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
    install(FILES LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/everything-lite)
    set(CPACK_PACKAGE_NAME "everything-lite")
    set(CPACK_PACKAGE_VENDOR "umdreamer")
    set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
    set(CPACK_PACKAGE_CONTACT "https://github.com/umdreamer/EverythingLite/issues")
    set(CPACK_DEBIAN_PACKAGE_MAINTAINER "umdreamer <15782682+umdreamer@users.noreply.github.com>")
    set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Local file name and path search with Qt")
    set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_SOURCE_DIR}/LICENSE")
    set(CPACK_GENERATOR "DEB")
    set(CPACK_PACKAGING_INSTALL_PREFIX "/usr")
    set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
    set(CPACK_DEBIAN_PACKAGE_SECTION "utils")
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
    set(CPACK_DEBIAN_PACKAGE_DEPENDS "qt6-qpa-plugins, qt6-wayland, xdg-utils")
    set(CPACK_STRIP_FILES ON)
    include(CPack)
endif()
