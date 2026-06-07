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
    ${CMAKE_SOURCE_DIR}/../src/settings.cpp
    ${CMAKE_SOURCE_DIR}/stubs/terminalview.cpp
)
# Force-include the stub terminalview.h before any source file.
# Uses the same include guard (TERMINALVIEW_H) as the real header,
# so #include "terminalview.h" in sessionmanager.cpp is a no-op.
target_compile_options(tst_session_persistence PRIVATE
    -include${CMAKE_SOURCE_DIR}/stubs/terminalview.h
)
# Stubs dir in system path so #include <QQmlListProperty> finds our stub
target_include_directories(tst_session_persistence PRIVATE SYSTEM
    ${CMAKE_SOURCE_DIR}/stubs
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

# --- Word boundary (isWordChar) tests ---
add_executable(tst_word_boundary
    tst_word_boundary.cpp
    ${CMAKE_SOURCE_DIR}/../src/textutil.cpp
)
target_link_libraries(tst_word_boundary PRIVATE Qt5::Core Qt5::Test)
add_test(NAME WordBoundary COMMAND tst_word_boundary)
