cmake_minimum_required(VERSION 3.16)
project(ghosteel-tests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt5 REQUIRED COMPONENTS Core Gui Test Network)

enable_testing()

# Common definitions for test builds
add_definitions(
    -DAPP_NAME=\"test-app\"
    -DAPP_ORG=\"test-org\"
    -DAPP_QML_MODULE=\"test.qml.module\"
    -DGHOSTTY_STATIC
)

include_directories(${CMAKE_SOURCE_DIR}/../src)
include_directories(${CMAKE_SOURCE_DIR}/../ghostty/include)

# --- Session persistence tests ---
add_executable(tst_session_persistence
    tst_session_persistence.cpp
    ${CMAKE_SOURCE_DIR}/../src/sessionmanager.cpp
    ${CMAKE_SOURCE_DIR}/../src/sessionstore.cpp
    ${CMAKE_SOURCE_DIR}/../src/singleinstance.cpp
    ${CMAKE_SOURCE_DIR}/../src/settings.cpp
    ${CMAKE_SOURCE_DIR}/../src/scrollencryptor.cpp
    ${CMAKE_SOURCE_DIR}/stubs/terminalview.cpp
)
# Force-include the stub terminalview.h before any source file.
# Uses the same include guard (TERMINALVIEW_H) as the real header,
# so #include "terminalview.h" in sessionmanager.cpp is a no-op.
target_compile_options(tst_session_persistence PRIVATE
    -include${CMAKE_SOURCE_DIR}/stubs/terminalview.h
)
target_link_libraries(tst_session_persistence PRIVATE Qt5::Core Qt5::Gui Qt5::Test Qt5::Network)
add_test(NAME SessionPersistence COMMAND tst_session_persistence)

# --- Settings tests ---
add_executable(tst_settings
    tst_settings.cpp
    ${CMAKE_SOURCE_DIR}/../src/settings.cpp
)
target_link_libraries(tst_settings PRIVATE Qt5::Core Qt5::Test)
add_test(NAME Settings COMMAND tst_settings)

# --- Settings behavior tests ---
add_executable(tst_settings_behavior
    tst_settings_behavior.cpp
    ${CMAKE_SOURCE_DIR}/../src/settings.cpp
)
target_link_libraries(tst_settings_behavior PRIVATE Qt5::Core Qt5::Test)
add_test(NAME SettingsBehavior COMMAND tst_settings_behavior)

# --- PtyReaderThread tests ---
add_executable(tst_pty_reader
    tst_pty_reader.cpp
    ${CMAKE_SOURCE_DIR}/../src/ptymanager.cpp
)
target_link_libraries(tst_pty_reader PRIVATE Qt5::Core Qt5::Test)
add_test(NAME PtyReader COMMAND tst_pty_reader)

# --- Key mapping tests ---
add_executable(tst_key_mapping
    tst_key_mapping.cpp
    ${CMAKE_SOURCE_DIR}/../src/keymapping.cpp
)
target_link_libraries(tst_key_mapping PRIVATE Qt5::Core Qt5::Test)
add_test(NAME KeyMapping COMMAND tst_key_mapping)

# --- TextUtil tests ---
add_executable(tst_textutil
    tst_textutil.cpp
    ${CMAKE_SOURCE_DIR}/../src/textutil.cpp
)
target_link_libraries(tst_textutil PRIVATE Qt5::Core Qt5::Test)
add_test(NAME TextUtil COMMAND tst_textutil)

# --- OSC 777 notification scanner tests ---
add_executable(tst_osc777
    tst_osc777.cpp
    ${CMAKE_SOURCE_DIR}/../src/ghosttyvt.cpp
    ${CMAKE_SOURCE_DIR}/stubs/ghostty_stubs.cpp
)
# No -include stub needed — GhosttyVt doesn't depend on TerminalView.
# We compile ghosttyvt.cpp directly and link against Ghostty C API stubs.
target_include_directories(tst_osc777 PRIVATE SYSTEM
    ${CMAKE_SOURCE_DIR}/stubs
)
target_link_libraries(tst_osc777 PRIVATE Qt5::Core Qt5::Test)
add_test(NAME Osc777 COMMAND tst_osc777)

# --- OSC 52 clipboard scanner tests ---
add_executable(tst_osc52
    tst_osc52.cpp
    ${CMAKE_SOURCE_DIR}/../src/ghosttyvt.cpp
    ${CMAKE_SOURCE_DIR}/stubs/ghostty_stubs.cpp
)
target_include_directories(tst_osc52 PRIVATE SYSTEM
    ${CMAKE_SOURCE_DIR}/stubs
)
target_link_libraries(tst_osc52 PRIVATE Qt5::Core Qt5::Test)
add_test(NAME Osc52 COMMAND tst_osc52)

# --- Word boundary (isWordChar) tests ---
add_executable(tst_word_boundary
    tst_word_boundary.cpp
    ${CMAKE_SOURCE_DIR}/../src/textutil.cpp
)
target_link_libraries(tst_word_boundary PRIVATE Qt5::Core Qt5::Test)
add_test(NAME WordBoundary COMMAND tst_word_boundary)

# --- Scrollback format parsing tests ---
add_executable(tst_scrollback_format
    tst_scrollback_format.cpp
    ${CMAKE_SOURCE_DIR}/../src/ghosttyvt.cpp
    ${CMAKE_SOURCE_DIR}/stubs/ghostty_stubs.cpp
)
target_include_directories(tst_scrollback_format PRIVATE SYSTEM
    ${CMAKE_SOURCE_DIR}/stubs
)
target_link_libraries(tst_scrollback_format PRIVATE Qt5::Core Qt5::Test)
add_test(NAME ScrollbackFormat COMMAND tst_scrollback_format)

# --- ScrollEncryptor (padding + format detection) tests ---
add_executable(tst_scrollencryptor
    tst_scrollencryptor.cpp
    ${CMAKE_SOURCE_DIR}/../src/scrollencryptor.cpp
)
target_link_libraries(tst_scrollencryptor PRIVATE Qt5::Core Qt5::Test)
add_test(NAME ScrollEncryptor COMMAND tst_scrollencryptor)

# --- URL detection tests ---
add_executable(tst_url_detection
    tst_url_detection.cpp
    ${CMAKE_SOURCE_DIR}/../src/textutil.cpp
)
target_link_libraries(tst_url_detection PRIVATE Qt5::Core Qt5::Test)
add_test(NAME UrlDetection COMMAND tst_url_detection)

# --- Default terminal modes (grapheme cluster, cursor blinking) tests ---
add_executable(tst_default_modes
    tst_default_modes.cpp
    ${CMAKE_SOURCE_DIR}/../src/ghosttyvt.cpp
    ${CMAKE_SOURCE_DIR}/stubs/ghostty_stubs.cpp
)
target_include_directories(tst_default_modes PRIVATE SYSTEM
    ${CMAKE_SOURCE_DIR}/stubs
)
target_link_libraries(tst_default_modes PRIVATE Qt5::Core Qt5::Test)
add_test(NAME DefaultModes COMMAND tst_default_modes)

# --- Terminal character width (East Asian Width) tests ---
add_executable(tst_terminal_width
    tst_terminal_width.cpp
)
target_link_libraries(tst_terminal_width PRIVATE Qt5::Core Qt5::Test)
add_test(NAME TerminalWidth COMMAND tst_terminal_width)

# --- Kitty PNG dimension-cap guard tests ---
add_executable(tst_kitty_image_guard
    tst_kitty_image_guard.cpp
)
target_link_libraries(tst_kitty_image_guard PRIVATE Qt5::Core Qt5::Test)
add_test(NAME KittyImageGuard COMMAND tst_kitty_image_guard)

# --- Glyph cache key (cluster + single-cp) logic tests ---
add_executable(tst_glyph_cache_key
    tst_glyph_cache_key.cpp
)
target_link_libraries(tst_glyph_cache_key PRIVATE Qt5::Core Qt5::Gui Qt5::Test)
add_test(NAME GlyphCacheKey COMMAND tst_glyph_cache_key)

# --- Compile smoke: app GUI sources -----------------------------------------
# The unit tests above deliberately compile most sources against stubs, so
# type drift in the GUI-only translation units (renderer, view, atlas) is
# otherwise caught only by the CI RPM build. This object library compiles
# them for real; nothing links it, so no display or Sailfish-only runtime
# deps are needed. bellfeedback.cpp (NgfClient) and ghosteel.cpp
# (sailfishapp) stay out: their headers are Sailfish-only and their logic
# is thin. Add new GUI sources here when they appear.
find_package(Qt5 COMPONENTS Quick OpenGL Dbus QUIET)
if(Qt5Quick_FOUND AND Qt5OpenGL_FOUND AND Qt5DBus_FOUND)
    add_library(compile_smoke OBJECT
        ${CMAKE_SOURCE_DIR}/../src/glrenderer.cpp
        ${CMAKE_SOURCE_DIR}/../src/glrenderer_geometry.cpp
        ${CMAKE_SOURCE_DIR}/../src/glrenderer_kitty.cpp
        ${CMAKE_SOURCE_DIR}/../src/glrenderer_pipeline.cpp
        ${CMAKE_SOURCE_DIR}/../src/glrenderer_shaders.cpp
        ${CMAKE_SOURCE_DIR}/../src/glyphatlas.cpp
        ${CMAKE_SOURCE_DIR}/../src/terminalview.cpp
        ${CMAKE_SOURCE_DIR}/../src/terminalview_touch.cpp
        ${CMAKE_SOURCE_DIR}/../src/terminalview_search.cpp
        ${CMAKE_SOURCE_DIR}/../src/terminalview_links.cpp
        ${CMAKE_SOURCE_DIR}/../src/keymapping.cpp
        ${CMAKE_SOURCE_DIR}/../src/ghosttyvt.cpp
        ${CMAKE_SOURCE_DIR}/../src/ptymanager.cpp
        ${CMAKE_SOURCE_DIR}/../src/sessionmanager.cpp
        ${CMAKE_SOURCE_DIR}/../src/sessionstore.cpp
        ${CMAKE_SOURCE_DIR}/../src/singleinstance.cpp
        ${CMAKE_SOURCE_DIR}/../src/scrollencryptor.cpp
        ${CMAKE_SOURCE_DIR}/../src/settings.cpp
        ${CMAKE_SOURCE_DIR}/../src/ghosteeladapter.cpp
    )
    target_compile_definitions(compile_smoke PRIVATE
        GHOSTTY_STATIC
    )
    target_link_libraries(compile_smoke PRIVATE
        Qt5::Core Qt5::Gui Qt5::Quick Qt5::OpenGL Qt5::DBus
    )
else()
    message(STATUS "Qt Quick/OpenGL dev packages not found; skipping compile_smoke")
endif()
