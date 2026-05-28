# NOTICE:
#
# Application name defined in TARGET has a corresponding QML filename.
# If name defined in TARGET is changed, the following needs to be done
# to match new name:
#   - corresponding QML filename must be changed
#   - desktop icon filename must be changed
#   - desktop filename must be changed
#   - icon definition filename in desktop file must be changed

# The name of your application
TARGET = ghosteel

CONFIG += sailfishapp

QT += gui quick qml

# Centralized app identity — change these to rename the app
APP_NAME = $$TARGET
APP_ORG = harbour.ghosteel
APP_QML_MODULE = harbour.ghosteel
DEFINES += APP_NAME=\\\"$$APP_NAME\\\"
DEFINES += APP_ORG=\\\"$$APP_ORG\\\"
DEFINES += APP_QML_MODULE=\\\"$$APP_QML_MODULE\\\"

# Version from git tag (CI builds use sed-replaced fallback)
GIT_VERSION = $$system(git describe --tags --always 2>/dev/null)
isEmpty(GIT_VERSION): GIT_VERSION = "dev"
DEFINES += GIT_VERSION=\\\"$$GIT_VERSION\\\"

# Ghostty submodule version
GHOSTTY_VERSION = $$system(cd ghostty && git describe --tags --always 2>/dev/null)
isEmpty(GHOSTTY_VERSION): GHOSTTY_VERSION = "unknown"
DEFINES += GHOSTTY_VERSION=\\\"$$GHOSTTY_VERSION\\\"

# libghostty integration
DEFINES += GHOSTTY_STATIC
INCLUDEPATH += ghostty/include

# Select pre-built library based on target architecture
equals(QT_ARCH, arm64) {
    LIBS += -L$$PWD/lib/aarch64 -lghostty-vt
} else:equals(QT_ARCH, arm) {
    LIBS += -L$$PWD/lib/armv7hl -lghostty-vt
} else {
    LIBS += -L$$PWD/lib/i486 -lghostty-vt
}

LIBS += -lpthread -lm -ldl -lutil -lrt

HEADERS += \
    src/ghosttyvt.h \
    src/ptymanager.h \
    src/sessionmanager.h \
    src/settings.h \
    src/terminalview.h

SOURCES += \
    src/ghosteel.cpp \
    src/ghosttyvt.cpp \
    src/ptymanager.cpp \
    src/sessionmanager.cpp \
    src/settings.cpp \
    src/terminalview.cpp

DISTFILES += qml/ghosteel.qml \
    LICENSE \
    qml/cover/CoverPage.qml \
    qml/pages/FirstPage.qml \
    qml/pages/SessionPage.qml \
    qml/pages/SettingsPage.qml \
    rpm/ghosteel.changes.in \
    rpm/ghosteel.changes.run.in \
    rpm/ghosteel.spec \
    ghosteel.desktop

SAILFISHAPP_ICONS = 86x86 108x108 128x128 172x172

# Shell integration scripts — copy from Ghostty submodule into app resources
shell_integration.files = $$PWD/ghostty/src/shell-integration/bash \
                          $$PWD/ghostty/src/shell-integration/zsh \
                          $$PWD/ghostty/src/shell-integration/fish
shell_integration.path = /usr/share/$${APP_NAME}/shell-integration
INSTALLS += shell_integration
