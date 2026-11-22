cmake_minimum_required(VERSION 3.22)

project(BeatrixLeslieIDE VERSION 1.0.1 LANGUAGES CXX)

add_compile_definitions(SPELL_APP_VERSION="${PROJECT_VERSION}")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_MACOSX_RPATH ON)
set(CMAKE_BUILD_WITH_INSTALL_RPATH OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(SPELL_WARNINGS_AS_ERRORS "Treat C++ compiler warnings as errors" ON)

# Automatically help CMake locate Homebrew Qt 6 and QScintilla2 on macOS,
# and honor environment Qt / chainload prefixes on Windows and Linux.
if(DEFINED QT_Chainload AND EXISTS "${QT_Chainload}")
    list(APPEND CMAKE_PREFIX_PATH "${QT_Chainload}")
endif()

foreach(_spell_env_prefix_var CMAKE_PREFIX_PATH QT_ROOT_DIR Qt6_DIR)
    if(DEFINED ENV{${_spell_env_prefix_var}} AND NOT "$ENV{${_spell_env_prefix_var}}" STREQUAL "")
        file(TO_CMAKE_PATH "$ENV{${_spell_env_prefix_var}}" _spell_env_paths)
        foreach(_spell_env_p ${_spell_env_paths})
            if(EXISTS "${_spell_env_p}")
                list(APPEND CMAKE_PREFIX_PATH "${_spell_env_p}")
            endif()
        endforeach()
        unset(_spell_env_p)
        unset(_spell_env_paths)
    endif()
endforeach()
unset(_spell_env_prefix_var)

if(APPLE)
    set(_spell_prefer_intel_brew FALSE)
    if("x86_64" IN_LIST CMAKE_OSX_ARCHITECTURES AND NOT "arm64" IN_LIST CMAKE_OSX_ARCHITECTURES)
        set(_spell_prefer_intel_brew TRUE)
    elseif(NOT CMAKE_OSX_ARCHITECTURES AND CMAKE_HOST_SYSTEM_PROCESSOR STREQUAL "x86_64")
        set(_spell_prefer_intel_brew TRUE)
    endif()

    if(_spell_prefer_intel_brew)
        set(_spell_brew_roots "/usr/local" "/opt/homebrew")
    else()
        set(_spell_brew_roots "/opt/homebrew" "/usr/local")
    endif()

    foreach(_spell_brew_root ${_spell_brew_roots})
        foreach(_spell_brew_formula qtbase qtsvg qscintilla2 qt)
            set(_spell_brew_prefix "${_spell_brew_root}/opt/${_spell_brew_formula}")
            if(EXISTS "${_spell_brew_prefix}")
                list(APPEND CMAKE_PREFIX_PATH "${_spell_brew_prefix}")
            endif()
        endforeach()
    endforeach()

    # Homebrew installs Qt6Svg in a separate prefix from Qt6Config.cmake (qtbase),
    # and Qt6Config searches sibling components with NO_DEFAULT_PATH unless listed here.
    foreach(_spell_brew_root ${_spell_brew_roots})
        set(_spell_qtsvg_prefix "${_spell_brew_root}/opt/qtsvg")
        if(EXISTS "${_spell_qtsvg_prefix}")
            list(APPEND QT_ADDITIONAL_PACKAGES_PREFIX_PATH "${_spell_qtsvg_prefix}")
        endif()
    endforeach()

    unset(_spell_prefer_intel_brew)
    unset(_spell_brew_roots)
    unset(_spell_brew_root)
    unset(_spell_brew_formula)
    unset(_spell_brew_prefix)
    unset(_spell_qtsvg_prefix)
endif()

# ---------------------------------------------------------------------------
# Qt 6
# ---------------------------------------------------------------------------
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Svg Test)

set(SPELL_QT_LIB_DIRS "")
set(SPELL_QT_PREFIX_DIRS "")
foreach(_spell_qt_pkg_dir "${Qt6_DIR}" "${Qt6Svg_DIR}")
    if(_spell_qt_pkg_dir)
        get_filename_component(_spell_qt_candidate_lib "${_spell_qt_pkg_dir}/../.." ABSOLUTE)
        if(EXISTS "${_spell_qt_candidate_lib}")
            list(APPEND SPELL_QT_LIB_DIRS "${_spell_qt_candidate_lib}")
        endif()
        get_filename_component(_spell_qt_candidate_prefix "${_spell_qt_pkg_dir}/../../.." ABSOLUTE)
        if(EXISTS "${_spell_qt_candidate_prefix}")
            list(APPEND SPELL_QT_PREFIX_DIRS "${_spell_qt_candidate_prefix}")
        endif()
    endif()
endforeach()
list(REMOVE_DUPLICATES SPELL_QT_LIB_DIRS)
list(REMOVE_DUPLICATES SPELL_QT_PREFIX_DIRS)
unset(_spell_qt_pkg_dir)
unset(_spell_qt_candidate_lib)
unset(_spell_qt_candidate_prefix)

# ---------------------------------------------------------------------------
# QScintilla2 (Qt 6)
# ---------------------------------------------------------------------------
find_path(QSCINTILLA_INCLUDE_DIR
    NAMES Qsci/qsciscintilla.h
    HINTS
        ${CMAKE_PREFIX_PATH}
        ${SPELL_QT_PREFIX_DIRS}
        /opt/homebrew/opt/qscintilla2
        /usr/local/opt/qscintilla2
    PATH_SUFFIXES
        include
        include/qt6
        include/x86_64-linux-gnu/qt6
        include/aarch64-linux-gnu/qt6
    PATHS
        /opt/homebrew/opt/qscintilla2/include
        /usr/local/opt/qscintilla2/include
        /usr/include
        /usr/include/qt6
        /usr/local/include
)

find_library(QSCINTILLA_LIBRARY
    NAMES
        qscintilla2_qt6
        qscintilla2-qt6
        qscintilla2_qt6d
        qscintilla2
    HINTS
        ${CMAKE_PREFIX_PATH}
        ${SPELL_QT_PREFIX_DIRS}
        ${SPELL_QT_LIB_DIRS}
        /opt/homebrew/opt/qscintilla2
        /usr/local/opt/qscintilla2
    PATH_SUFFIXES
        lib
        lib64
        lib/x86_64-linux-gnu
        lib/aarch64-linux-gnu
    PATHS
        /opt/homebrew/opt/qscintilla2/lib
        /usr/local/opt/qscintilla2/lib
        /usr/lib
        /usr/lib64
        /usr/local/lib
)

if(NOT QSCINTILLA_INCLUDE_DIR OR NOT QSCINTILLA_LIBRARY)
    message(FATAL_ERROR
        "QScintilla2 for Qt 6 was not found.\n"
        "  QSCINTILLA_INCLUDE_DIR = ${QSCINTILLA_INCLUDE_DIR}\n"
        "  QSCINTILLA_LIBRARY     = ${QSCINTILLA_LIBRARY}\n"
        "Install QScintilla2 for Qt 6 (for example: 'brew install qscintilla2' on macOS, "
        "'sudo apt install libqscintilla2-qt6-dev' on Debian/Ubuntu, or run "
        "'scripts/install_qscintilla_windows.ps1' on Windows), "
        "or pass -DQSCINTILLA_INCLUDE_DIR=<path> -DQSCINTILLA_LIBRARY=<path> or extend CMAKE_PREFIX_PATH."
    )
endif()

get_filename_component(QSCINTILLA_LIB_DIR "${QSCINTILLA_LIBRARY}" DIRECTORY)

if(WIN32)
    unset(QSCINTILLA_RUNTIME_DLL)
    find_file(QSCINTILLA_RUNTIME_DLL
        NAMES
            qscintilla2_qt6.dll
            qscintilla2-qt6.dll
            qscintilla2_qt6d.dll
            qscintilla2-qt6d.dll
        HINTS
            "${QSCINTILLA_LIB_DIR}"
            "${QSCINTILLA_LIB_DIR}/../bin"
            ${CMAKE_PREFIX_PATH}
            ${SPELL_QT_PREFIX_DIRS}
        PATH_SUFFIXES
            ""
            bin
            lib
    )
endif()

# ---------------------------------------------------------------------------
# spell_core Rust Core Library
# ---------------------------------------------------------------------------
option(SPELL_BUILD_RUST "Invoke cargo build --release for spell_core automatically during build" ON)

set(SPELL_CORE_DIR "${CMAKE_SOURCE_DIR}/rust/spell_core")
set(SPELL_CORE_TARGET_DIR "${SPELL_CORE_DIR}/target/release")

if(APPLE)
    set(SPELL_CORE_EXPECTED_LIB "${SPELL_CORE_TARGET_DIR}/libspell_core.dylib")
    set(SPELL_CORE_SEARCH_NAMES "libspell_core.dylib")
    set(SPELL_CORE_RUNTIME_LIBRARY "${SPELL_CORE_EXPECTED_LIB}")
elseif(WIN32)
    if(MINGW)
        set(SPELL_CORE_EXPECTED_LIB "${SPELL_CORE_TARGET_DIR}/libspell_core.dll.a")
        set(SPELL_CORE_SEARCH_NAMES "libspell_core.dll.a" "spell_core.dll.lib" "spell_core.lib")
    else()
        set(SPELL_CORE_EXPECTED_LIB "${SPELL_CORE_TARGET_DIR}/spell_core.dll.lib")
        set(SPELL_CORE_SEARCH_NAMES "spell_core.dll.lib" "spell_core.lib" "libspell_core.dll.a")
    endif()
    set(SPELL_CORE_RUNTIME_LIBRARY "${SPELL_CORE_TARGET_DIR}/spell_core.dll")
else()
    set(SPELL_CORE_EXPECTED_LIB "${SPELL_CORE_TARGET_DIR}/libspell_core.so")
    set(SPELL_CORE_SEARCH_NAMES "libspell_core.so")
    set(SPELL_CORE_RUNTIME_LIBRARY "${SPELL_CORE_EXPECTED_LIB}")
endif()

find_library(SPELL_CORE_LIBRARY
    NAMES ${SPELL_CORE_SEARCH_NAMES}
    PATHS "${SPELL_CORE_TARGET_DIR}"
    NO_DEFAULT_PATH
)

if(NOT SPELL_CORE_LIBRARY)
    set(SPELL_CORE_LIBRARY "${SPELL_CORE_EXPECTED_LIB}" CACHE FILEPATH
        "Path to the spell_core library in rust/spell_core/target/release" FORCE)
    if(NOT SPELL_BUILD_RUST)
        message(WARNING
            "spell_core library was not found at ${SPELL_CORE_EXPECTED_LIB}.\n"
            "Build it before linking by running:\n"
            "  cargo build --release --manifest-path \"${SPELL_CORE_DIR}/Cargo.toml\"\n"
            "or run 'cmake --build <build-dir> --target spell_build_rust', "
            "or reconfigure with -DSPELL_BUILD_RUST=ON."
        )
    endif()
endif()

if(WIN32 AND NOT SPELL_BUILD_RUST AND NOT SPELL_CORE_LIBRARY MATCHES "\\.dll\\.(lib|a)$" AND NOT EXISTS "${SPELL_CORE_RUNTIME_LIBRARY}")
    set(SPELL_CORE_RUNTIME_LIBRARY "")
endif()

if(APPLE)
    find_program(INSTALL_NAME_TOOL_EXECUTABLE install_name_tool)
    find_program(CODESIGN_EXECUTABLE codesign)
    find_program(LIPO_EXECUTABLE lipo)
endif()

find_program(CARGO_EXECUTABLE cargo)

set(_spell_osx_has_arm64 FALSE)
set(_spell_osx_has_x86_64 FALSE)
if(APPLE)
    if("arm64" IN_LIST CMAKE_OSX_ARCHITECTURES)
        set(_spell_osx_has_arm64 TRUE)
    endif()
    if("x86_64" IN_LIST CMAKE_OSX_ARCHITECTURES)
        set(_spell_osx_has_x86_64 TRUE)
    endif()
endif()

if(APPLE AND _spell_osx_has_arm64 AND _spell_osx_has_x86_64)
    if(SPELL_BUILD_RUST AND NOT LIPO_EXECUTABLE)
        message(FATAL_ERROR "Universal2 build requested (CMAKE_OSX_ARCHITECTURES='${CMAKE_OSX_ARCHITECTURES}'), but 'lipo' was not found in PATH.")
    endif()
    set(_spell_rust_build_cmds
        COMMAND "${CARGO_EXECUTABLE}" build --release --lib --target aarch64-apple-darwin --target-dir "${SPELL_CORE_DIR}/target"
        COMMAND "${CARGO_EXECUTABLE}" build --release --lib --target x86_64-apple-darwin --target-dir "${SPELL_CORE_DIR}/target"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${SPELL_CORE_TARGET_DIR}"
        COMMAND "${LIPO_EXECUTABLE}" -create
            "${SPELL_CORE_DIR}/target/aarch64-apple-darwin/release/libspell_core.dylib"
            "${SPELL_CORE_DIR}/target/x86_64-apple-darwin/release/libspell_core.dylib"
            -output "${SPELL_CORE_TARGET_DIR}/libspell_core.dylib"
    )
elseif(APPLE AND _spell_osx_has_arm64)
    set(_spell_rust_build_cmds
        COMMAND "${CARGO_EXECUTABLE}" build --release --lib --target aarch64-apple-darwin --target-dir "${SPELL_CORE_DIR}/target"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${SPELL_CORE_TARGET_DIR}"
        COMMAND "${CMAKE_COMMAND}" -E copy
            "${SPELL_CORE_DIR}/target/aarch64-apple-darwin/release/libspell_core.dylib"
            "${SPELL_CORE_TARGET_DIR}/libspell_core.dylib"
    )
elseif(APPLE AND _spell_osx_has_x86_64)
    set(_spell_rust_build_cmds
        COMMAND "${CARGO_EXECUTABLE}" build --release --lib --target x86_64-apple-darwin --target-dir "${SPELL_CORE_DIR}/target"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${SPELL_CORE_TARGET_DIR}"
        COMMAND "${CMAKE_COMMAND}" -E copy
            "${SPELL_CORE_DIR}/target/x86_64-apple-darwin/release/libspell_core.dylib"
            "${SPELL_CORE_TARGET_DIR}/libspell_core.dylib"
    )
else()
    set(_spell_rust_build_cmds
        COMMAND "${CARGO_EXECUTABLE}" build --release --lib --target-dir "${SPELL_CORE_DIR}/target"
    )
endif()

set(_spell_rust_post_cmds "")
if(APPLE)
    if(INSTALL_NAME_TOOL_EXECUTABLE)
        list(APPEND _spell_rust_post_cmds
            COMMAND "${INSTALL_NAME_TOOL_EXECUTABLE}" -id "@rpath/libspell_core.dylib" "${SPELL_CORE_RUNTIME_LIBRARY}"
        )
    endif()
    if(CODESIGN_EXECUTABLE)
        list(APPEND _spell_rust_post_cmds
            COMMAND "${CODESIGN_EXECUTABLE}" --force --sign - "${SPELL_CORE_RUNTIME_LIBRARY}"
        )
    endif()
endif()

if(SPELL_BUILD_RUST)
    if(NOT CARGO_EXECUTABLE)
        message(FATAL_ERROR "SPELL_BUILD_RUST is ON, but 'cargo' was not found in PATH.")
    endif()
    set(_spell_rust_byproducts "${SPELL_CORE_LIBRARY}")
    if(SPELL_CORE_RUNTIME_LIBRARY)
        list(APPEND _spell_rust_byproducts "${SPELL_CORE_RUNTIME_LIBRARY}")
    endif()
    list(REMOVE_DUPLICATES _spell_rust_byproducts)
    add_custom_target(spell_build_rust ALL
        ${_spell_rust_build_cmds}
        ${_spell_rust_post_cmds}
        WORKING_DIRECTORY "${SPELL_CORE_DIR}"
        BYPRODUCTS ${_spell_rust_byproducts}
        COMMENT "Building spell_core Rust library (release)"
        VERBATIM
    )
    unset(_spell_rust_byproducts)
else()
    if(CARGO_EXECUTABLE)
        add_custom_target(spell_build_rust
            ${_spell_rust_build_cmds}
            ${_spell_rust_post_cmds}
            WORKING_DIRECTORY "${SPELL_CORE_DIR}"
            COMMENT "Building spell_core Rust library (release)"
            VERBATIM
        )
    else()
        add_custom_target(spell_build_rust
            COMMAND "${CMAKE_COMMAND}" -E echo "Error: 'cargo' was not found in PATH; install Rust to build spell_core."
            COMMAND "${CMAKE_COMMAND}" -E false
            COMMENT "Cannot build spell_core without cargo"
        )
    endif()
endif()
unset(_spell_osx_has_arm64)
unset(_spell_osx_has_x86_64)
unset(_spell_rust_build_cmds)
unset(_spell_rust_post_cmds)

# On Linux/ELF, Rust cdylibs omit DT_SONAME by default; using IMPORTED_NO_SONAME
# ensures the linker records bare 'libspell_core.so' in DT_NEEDED rather than a
# build-machine absolute path.
if(UNIX AND NOT APPLE)
    add_library(SpellCoreShared SHARED IMPORTED GLOBAL)
    set_target_properties(SpellCoreShared PROPERTIES
        IMPORTED_LOCATION "${SPELL_CORE_LIBRARY}"
        IMPORTED_NO_SONAME TRUE
    )
    if(SPELL_BUILD_RUST)
        add_dependencies(SpellCoreShared spell_build_rust)
    endif()
    set(SPELL_CORE_LINK_TARGET SpellCoreShared)
else()
    set(SPELL_CORE_LINK_TARGET "${SPELL_CORE_LIBRARY}")
endif()

# ---------------------------------------------------------------------------
# SpellCoreLib (all BeatrixLeslie IDE C++ sources except main.cpp and tests)
# ---------------------------------------------------------------------------
add_library(SpellCoreLib STATIC
    # Core
    cpp/core/SpellModels.h
    cpp/core/SpellModels.cpp
    cpp/core/SpellCoreClient.h
    cpp/core/SpellCoreClient.cpp
    cpp/core/SpellSettingsStore.h
    cpp/core/SpellSettingsStore.cpp
    cpp/core/SpellCitationLinkifier.h
    cpp/core/SpellCitationLinkifier.cpp

    # Theme
    cpp/theme/SpellTheme.h
    cpp/theme/SpellTheme.cpp
    cpp/theme/SpellIcons.h
    cpp/theme/SpellIcons.cpp

    # Reusable Widgets
    cpp/widgets/SpellSplitHandle.h
    cpp/widgets/SpellSplitHandle.cpp
    cpp/widgets/SpellBadges.h
    cpp/widgets/SpellBadges.cpp
    cpp/widgets/SpellMarkdownView.h
    cpp/widgets/SpellMarkdownView.cpp
    cpp/widgets/SpellDiffWidget.h
    cpp/widgets/SpellDiffWidget.cpp

    # Integrated Terminal
    cpp/terminal/SpellPtyProcess.h
    cpp/terminal/SpellPtyProcess.cpp
    cpp/terminal/SpellTerminalEmulator.h
    cpp/terminal/SpellTerminalEmulator.cpp
    cpp/terminal/SpellTerminalSurface.h
    cpp/terminal/SpellTerminalSurface.cpp
    cpp/terminal/SpellTerminalPanel.h
    cpp/terminal/SpellTerminalPanel.cpp

    # Panes
    cpp/panes/EditorDocument.h
    cpp/panes/EditorDocument.cpp
    cpp/panes/RustCustomLexer.h
    cpp/panes/RustCustomLexer.cpp
    cpp/panes/CodeEditorPane.h
    cpp/panes/CodeEditorPane.cpp
    cpp/panes/ProjectExplorerPane.h
    cpp/panes/ProjectExplorerPane.cpp
    cpp/panes/ActiveSubagentsWidget.h
    cpp/panes/ActiveSubagentsWidget.cpp
    cpp/panes/ComposerWidget.h
    cpp/panes/ComposerWidget.cpp
    cpp/panes/ChatTimelineWidget.h
    cpp/panes/ChatTimelineWidget.cpp
    cpp/panes/AgentChatPane.h
    cpp/panes/AgentChatPane.cpp

    # Settings
    cpp/settings/GeneralSettingsPane.h
    cpp/settings/GeneralSettingsPane.cpp
    cpp/settings/EngineSettingsPane.h
    cpp/settings/EngineSettingsPane.cpp
    cpp/settings/StatisticsSettingsPane.h
    cpp/settings/StatisticsSettingsPane.cpp
    cpp/settings/SubagentsSettingsPane.h
    cpp/settings/SubagentsSettingsPane.cpp
    cpp/settings/ConversationSettingsPane.h
    cpp/settings/ConversationSettingsPane.cpp
    cpp/settings/AgentSettingsPane.h
    cpp/settings/AgentSettingsPane.cpp
    cpp/settings/OnlineResearchSettingsPane.h
    cpp/settings/OnlineResearchSettingsPane.cpp
    cpp/settings/TimersSettingsPane.h
    cpp/settings/TimersSettingsPane.cpp
    cpp/settings/TerminalSettingsPane.h
    cpp/settings/TerminalSettingsPane.cpp
    cpp/settings/DiagnosticsSettingsPane.h
    cpp/settings/DiagnosticsSettingsPane.cpp
    cpp/settings/AboutSettingsPane.h
    cpp/settings/AboutSettingsPane.cpp
    cpp/settings/SettingsDialog.h
    cpp/settings/SettingsDialog.cpp

    # Application Shell
    cpp/shell/WindowChrome.h
    cpp/shell/WindowChrome.cpp
    cpp/shell/MainWindow.h
    cpp/shell/MainWindow.cpp
)

target_include_directories(SpellCoreLib
    PUBLIC
        "${CMAKE_SOURCE_DIR}/cpp"
        "${CMAKE_SOURCE_DIR}/rust/spell_core/include"
)

target_include_directories(SpellCoreLib
    SYSTEM PUBLIC
        "${QSCINTILLA_INCLUDE_DIR}"
)

target_link_libraries(SpellCoreLib
    PUBLIC
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets
        Qt6::Svg
        "${QSCINTILLA_LIBRARY}"
        ${SPELL_CORE_LINK_TARGET}
)

if(UNIX AND NOT APPLE)
    target_link_libraries(SpellCoreLib PUBLIC util)
elseif(WIN32)
    target_link_libraries(SpellCoreLib PUBLIC kernel32)
    option(SPELL_QSCINTILLA_DLL "Define QSCINTILLA_DLL when linking dynamic QScintilla2 on Windows" ON)
    if(SPELL_QSCINTILLA_DLL)
        target_compile_definitions(SpellCoreLib PUBLIC QSCINTILLA_DLL)
    endif()
endif()

if(MSVC)
    target_compile_options(SpellCoreLib PUBLIC
        /utf-8
        /W4
        /external:W0
        $<$<BOOL:${SPELL_WARNINGS_AS_ERRORS}>:/WX>
    )
else()
    target_compile_options(SpellCoreLib PUBLIC
        -Wall
        -Wextra
        $<$<BOOL:${SPELL_WARNINGS_AS_ERRORS}>:-Werror>
    )
endif()

if(SPELL_BUILD_RUST)
    add_dependencies(SpellCoreLib spell_build_rust)
endif()

# ---------------------------------------------------------------------------
# Runtime Search Paths & Bundle Resources
# ---------------------------------------------------------------------------
set(SPELL_BUNDLE_RESOURCES
    "${CMAKE_SOURCE_DIR}/Resources/AppIcon.icns"
    "${CMAKE_SOURCE_DIR}/Resources/AppIconRounded.png"
    "${CMAKE_SOURCE_DIR}/Resources/AppLogo.jpg"
    "${CMAKE_SOURCE_DIR}/Resources/HatSilhouetteTemplate.png"
    "${CMAKE_SOURCE_DIR}/Resources/Credits.rtf"
    "${CMAKE_SOURCE_DIR}/Resources/THIRD_PARTY_LICENSES.txt"
)
if(EXISTS "${CMAKE_SOURCE_DIR}/Resources/HatSilhouetteTemplate@2x.png")
    list(APPEND SPELL_BUNDLE_RESOURCES "${CMAKE_SOURCE_DIR}/Resources/HatSilhouetteTemplate@2x.png")
endif()

if(APPLE)
    set(SPELL_MACOS_BUILD_RPATHS
        "@executable_path/../Frameworks"
        "${SPELL_CORE_TARGET_DIR}"
        ${SPELL_QT_LIB_DIRS}
    )
    if(QSCINTILLA_LIB_DIR)
        list(APPEND SPELL_MACOS_BUILD_RPATHS "${QSCINTILLA_LIB_DIR}")
    endif()
    foreach(_spell_macos_lib_dir
        "/opt/homebrew/opt/qscintilla2/lib"
        "/opt/homebrew/opt/qtbase/lib"
        "/opt/homebrew/opt/qtsvg/lib"
        "/usr/local/opt/qscintilla2/lib"
        "/usr/local/opt/qtbase/lib"
        "/usr/local/opt/qtsvg/lib"
    )
        if(EXISTS "${_spell_macos_lib_dir}")
            list(APPEND SPELL_MACOS_BUILD_RPATHS "${_spell_macos_lib_dir}")
        endif()
    endforeach()
    unset(_spell_macos_lib_dir)
    list(REMOVE_DUPLICATES SPELL_MACOS_BUILD_RPATHS)

    # Installed macOS bundle MUST NOT embed any build-machine paths.
    set(SPELL_MACOS_INSTALL_RPATHS "@executable_path/../Frameworks")
elseif(UNIX)
    set(SPELL_UNIX_BUILD_RPATHS
        "$ORIGIN"
        "$ORIGIN/../lib"
        "${SPELL_CORE_TARGET_DIR}"
        ${SPELL_QT_LIB_DIRS}
    )
    if(QSCINTILLA_LIB_DIR)
        list(APPEND SPELL_UNIX_BUILD_RPATHS "${QSCINTILLA_LIB_DIR}")
    endif()
    list(REMOVE_DUPLICATES SPELL_UNIX_BUILD_RPATHS)

    set(SPELL_UNIX_INSTALL_RPATHS "$ORIGIN;$ORIGIN/../lib")
endif()

# ---------------------------------------------------------------------------
# BeatrixLeslieIDE Application Executable
# ---------------------------------------------------------------------------
add_executable(BeatrixLeslieIDE MACOSX_BUNDLE WIN32
    cpp/main.cpp
)

target_link_libraries(BeatrixLeslieIDE
    PRIVATE
        SpellCoreLib
)

if(APPLE)
    set(SPELL_INFO_PLIST_TEMPLATE "${CMAKE_CURRENT_BINARY_DIR}/BeatrixLeslieIDE-Info.plist.in")
    file(WRITE "${SPELL_INFO_PLIST_TEMPLATE}" [=[<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleDevelopmentRegion</key>
    <string>en</string>
    <key>CFBundleDisplayName</key>
    <string>${MACOSX_BUNDLE_BUNDLE_NAME}</string>
    <key>CFBundleExecutable</key>
    <string>${MACOSX_BUNDLE_EXECUTABLE_NAME}</string>
    <key>CFBundleIconFile</key>
    <string>${MACOSX_BUNDLE_ICON_FILE}</string>
    <key>CFBundleIdentifier</key>
    <string>${MACOSX_BUNDLE_GUI_IDENTIFIER}</string>
    <key>CFBundleInfoDictionaryVersion</key>
    <string>6.0</string>
    <key>CFBundleName</key>
    <string>${MACOSX_BUNDLE_BUNDLE_NAME}</string>
    <key>CFBundlePackageType</key>
    <string>APPL</string>
    <key>CFBundleShortVersionString</key>
    <string>${MACOSX_BUNDLE_SHORT_VERSION_STRING}</string>
    <key>CFBundleVersion</key>
    <string>${MACOSX_BUNDLE_BUNDLE_VERSION}</string>
    <key>LSApplicationCategoryType</key>
    <string>public.app-category.developer-tools</string>
    <key>NSHighResolutionCapable</key>
    <true/>
    <key>NSHumanReadableCopyright</key>
    <string>${MACOSX_BUNDLE_COPYRIGHT}</string>
    <key>NSAppTransportSecurity</key>
    <dict>
        <key>NSAllowsArbitraryLoads</key>
        <true/>
        <key>NSAllowsLocalNetworking</key>
        <true/>
    </dict>
</dict>
</plist>
]=])

    set_target_properties(BeatrixLeslieIDE PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_INFO_PLIST "${SPELL_INFO_PLIST_TEMPLATE}"
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.spell.BeatrixLeslieIDE"
        MACOSX_BUNDLE_BUNDLE_NAME "BeatrixLeslie IDE"
        MACOSX_BUNDLE_ICON_FILE "AppIcon.icns"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
        MACOSX_BUNDLE_COPYRIGHT "Copyright © 2026 BeatrixLeslie IDE."
        BUILD_WITH_INSTALL_RPATH OFF
        BUILD_RPATH "${SPELL_MACOS_BUILD_RPATHS}"
        INSTALL_RPATH "${SPELL_MACOS_INSTALL_RPATHS}"
    )

    set(_spell_bundle_post_cmds
        COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_BUNDLE_CONTENT_DIR:BeatrixLeslieIDE>/Resources"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            ${SPELL_BUNDLE_RESOURCES}
            "$<TARGET_BUNDLE_CONTENT_DIR:BeatrixLeslieIDE>/Resources/"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_BUNDLE_CONTENT_DIR:BeatrixLeslieIDE>/Frameworks"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${SPELL_CORE_RUNTIME_LIBRARY}"
            "$<TARGET_BUNDLE_CONTENT_DIR:BeatrixLeslieIDE>/Frameworks/libspell_core.dylib"
    )
    if(INSTALL_NAME_TOOL_EXECUTABLE)
        list(APPEND _spell_bundle_post_cmds
            COMMAND "${INSTALL_NAME_TOOL_EXECUTABLE}" -id "@rpath/libspell_core.dylib"
                "$<TARGET_BUNDLE_CONTENT_DIR:BeatrixLeslieIDE>/Frameworks/libspell_core.dylib"
            COMMAND "${INSTALL_NAME_TOOL_EXECUTABLE}" -change
                "${SPELL_CORE_RUNTIME_LIBRARY}"
                "@rpath/libspell_core.dylib"
                "$<TARGET_FILE:BeatrixLeslieIDE>"
        )
    endif()
    if(CODESIGN_EXECUTABLE)
        list(APPEND _spell_bundle_post_cmds
            COMMAND "${CODESIGN_EXECUTABLE}" --force --sign -
                "$<TARGET_BUNDLE_CONTENT_DIR:BeatrixLeslieIDE>/Frameworks/libspell_core.dylib"
            COMMAND "${CODESIGN_EXECUTABLE}" --force --sign -
                "$<TARGET_BUNDLE_DIR:BeatrixLeslieIDE>"
        )
    endif()

    add_custom_command(TARGET BeatrixLeslieIDE POST_BUILD
        ${_spell_bundle_post_cmds}
        COMMENT "Bundling BeatrixLeslie IDE resources, libspell_core.dylib, and ad-hoc code signature"
        VERBATIM
    )
    unset(_spell_bundle_post_cmds)
else()
    add_custom_command(TARGET BeatrixLeslieIDE POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:BeatrixLeslieIDE>/Resources"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            ${SPELL_BUNDLE_RESOURCES}
            "$<TARGET_FILE_DIR:BeatrixLeslieIDE>/Resources/"
        COMMENT "Copying BeatrixLeslie IDE resources next to BeatrixLeslieIDE binary"
        VERBATIM
    )

    if(WIN32)
        set_target_properties(BeatrixLeslieIDE PROPERTIES
            BUILD_WITH_INSTALL_RPATH OFF
        )
        if(SPELL_CORE_RUNTIME_LIBRARY)
            add_custom_command(TARGET BeatrixLeslieIDE POST_BUILD
                COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    "${SPELL_CORE_RUNTIME_LIBRARY}"
                    "$<TARGET_FILE_DIR:BeatrixLeslieIDE>/"
                COMMENT "Copying spell_core.dll next to BeatrixLeslieIDE.exe"
                VERBATIM
            )
        endif()
        if(QSCINTILLA_RUNTIME_DLL)
            add_custom_command(TARGET BeatrixLeslieIDE POST_BUILD
                COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    "${QSCINTILLA_RUNTIME_DLL}"
                    "$<TARGET_FILE_DIR:BeatrixLeslieIDE>/"
                COMMENT "Copying QScintilla2 runtime DLL next to BeatrixLeslieIDE.exe"
                VERBATIM
            )
        endif()
    elseif(UNIX)
        set_target_properties(BeatrixLeslieIDE PROPERTIES
            BUILD_WITH_INSTALL_RPATH OFF
            BUILD_RPATH "${SPELL_UNIX_BUILD_RPATHS}"
            INSTALL_RPATH "${SPELL_UNIX_INSTALL_RPATHS}"
        )
    endif()
endif()

# ---------------------------------------------------------------------------
# SpellQtTests Executable & CTest Registration
# ---------------------------------------------------------------------------
enable_testing()

add_executable(SpellQtTests
    cpp/tests/SpellQtTests.cpp
)

target_link_libraries(SpellQtTests
    PRIVATE
        SpellCoreLib
        Qt6::Test
)

if(APPLE)
    set_target_properties(SpellQtTests PROPERTIES
        BUILD_WITH_INSTALL_RPATH OFF
        BUILD_RPATH "${SPELL_MACOS_BUILD_RPATHS}"
        INSTALL_RPATH "${SPELL_MACOS_INSTALL_RPATHS}"
    )
elseif(UNIX)
    set_target_properties(SpellQtTests PROPERTIES
        BUILD_WITH_INSTALL_RPATH OFF
        BUILD_RPATH "${SPELL_UNIX_BUILD_RPATHS}"
        INSTALL_RPATH "${SPELL_UNIX_INSTALL_RPATHS}"
    )
elseif(WIN32)
    set_target_properties(SpellQtTests PROPERTIES
        BUILD_WITH_INSTALL_RPATH OFF
    )
    if(SPELL_CORE_RUNTIME_LIBRARY)
        add_custom_command(TARGET SpellQtTests POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${SPELL_CORE_RUNTIME_LIBRARY}"
                "$<TARGET_FILE_DIR:SpellQtTests>/"
            COMMENT "Copying spell_core.dll next to SpellQtTests.exe"
            VERBATIM
        )
    endif()
    if(QSCINTILLA_RUNTIME_DLL)
        add_custom_command(TARGET SpellQtTests POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${QSCINTILLA_RUNTIME_DLL}"
                "$<TARGET_FILE_DIR:SpellQtTests>/"
            COMMENT "Copying QScintilla2 runtime DLL next to SpellQtTests.exe"
            VERBATIM
        )
    endif()
endif()

add_test(NAME SpellQtTests COMMAND SpellQtTests)
set_tests_properties(SpellQtTests PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;SPELL_EPHEMERAL_KEYRING=1"
)
if(WIN32)
    foreach(_spell_qt_prefix ${SPELL_QT_PREFIX_DIRS})
        if(EXISTS "${_spell_qt_prefix}/bin")
            set_property(TEST SpellQtTests APPEND PROPERTY
                ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${_spell_qt_prefix}/bin"
            )
        endif()
    endforeach()
    unset(_spell_qt_prefix)
endif()

# ---------------------------------------------------------------------------
# Portable Install Layout (cmake --install <build> --prefix <dist>)
# ---------------------------------------------------------------------------
install(TARGETS BeatrixLeslieIDE
    BUNDLE DESTINATION .
    RUNTIME DESTINATION bin
)

if(APPLE)
    install(FILES ${SPELL_BUNDLE_RESOURCES}
        DESTINATION "BeatrixLeslieIDE.app/Contents/Resources"
    )
    install(FILES "${SPELL_CORE_RUNTIME_LIBRARY}"
        DESTINATION "BeatrixLeslieIDE.app/Contents/Frameworks"
    )
    if(INSTALL_NAME_TOOL_EXECUTABLE)
        install(CODE "
            execute_process(
                COMMAND \"${INSTALL_NAME_TOOL_EXECUTABLE}\" -id \"@rpath/libspell_core.dylib\"
                    \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/BeatrixLeslieIDE.app/Contents/Frameworks/libspell_core.dylib\"
                RESULT_VARIABLE _spell_int_res
            )
            if(NOT _spell_int_res EQUAL 0)
                message(FATAL_ERROR \"install_name_tool failed on installed libspell_core.dylib\")
            endif()
        ")
    endif()
    if(CODESIGN_EXECUTABLE)
        install(CODE "
            execute_process(
                COMMAND \"${CODESIGN_EXECUTABLE}\" --force --sign -
                    \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/BeatrixLeslieIDE.app/Contents/Frameworks/libspell_core.dylib\"
                RESULT_VARIABLE _spell_cs_lib_res
            )
            if(NOT _spell_cs_lib_res EQUAL 0)
                message(FATAL_ERROR \"codesign failed on installed libspell_core.dylib\")
            endif()
            execute_process(
                COMMAND \"${CODESIGN_EXECUTABLE}\" --force --sign -
                    \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/BeatrixLeslieIDE.app\"
                RESULT_VARIABLE _spell_cs_app_res
            )
            if(NOT _spell_cs_app_res EQUAL 0)
                message(FATAL_ERROR \"codesign failed on installed BeatrixLeslieIDE.app\")
            endif()
        ")
    endif()
elseif(WIN32)
    if(SPELL_CORE_RUNTIME_LIBRARY)
        install(FILES "${SPELL_CORE_RUNTIME_LIBRARY}"
            DESTINATION bin
        )
    endif()
    if(QSCINTILLA_RUNTIME_DLL)
        install(FILES "${QSCINTILLA_RUNTIME_DLL}"
            DESTINATION bin
        )
    endif()
    install(FILES ${SPELL_BUNDLE_RESOURCES}
        DESTINATION bin/Resources
    )
elseif(UNIX)
    install(FILES "${SPELL_CORE_RUNTIME_LIBRARY}"
        DESTINATION lib
    )
    install(FILES ${SPELL_BUNDLE_RESOURCES}
        DESTINATION bin/Resources
    )
    install(FILES ${SPELL_BUNDLE_RESOURCES}
        DESTINATION share/BeatrixLeslieIDE/Resources
    )
    install(FILES "${CMAKE_SOURCE_DIR}/Resources/com.spell.BeatrixLeslieIDE.desktop"
        DESTINATION share/applications
    )
    install(FILES "${CMAKE_SOURCE_DIR}/Resources/AppIconRounded.png"
        DESTINATION share/icons/hicolor/512x512/apps
        RENAME com.spell.BeatrixLeslieIDE.png
    )
    install(FILES "${CMAKE_SOURCE_DIR}/Resources/AppIconRounded.png"
        DESTINATION share/icons/hicolor/256x256/apps
        RENAME com.spell.BeatrixLeslieIDE.png
    )
endif()

# ---------------------------------------------------------------------------
# Configuration Summary
# ---------------------------------------------------------------------------
message(STATUS "BeatrixLeslie IDE ${PROJECT_VERSION} configuration summary:")
message(STATUS "  Qt6 version:         ${Qt6_VERSION}")
message(STATUS "  QScintilla include:  ${QSCINTILLA_INCLUDE_DIR}")
message(STATUS "  QScintilla library:  ${QSCINTILLA_LIBRARY}")
if(WIN32)
    message(STATUS "  QScintilla runtime:  ${QSCINTILLA_RUNTIME_DLL}")
endif()
if(APPLE AND CMAKE_OSX_ARCHITECTURES)
    message(STATUS "  macOS architectures: ${CMAKE_OSX_ARCHITECTURES}")
endif()
message(STATUS "  spell_core library:  ${SPELL_CORE_LIBRARY}")
message(STATUS "  SPELL_BUILD_RUST:    ${SPELL_BUILD_RUST}")
message(STATUS "  Warnings as errors:  ${SPELL_WARNINGS_AS_ERRORS}")
