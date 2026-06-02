// Include terminalview.h BEFORE Qt headers to ensure Ghostty's C headers
// are parsed before Qt defines the `emit` macro (which conflicts with
// Ghostty's use of 'emit' as a struct field name).
#include "terminalview.h"
#include "sessionmanager.h"
#include "settings.h"

#ifdef QT_QML_DEBUG
#include <QtQuick>
#endif

#include <sailfishapp.h>
#include <QGuiApplication>
#include <QQuickView>
#include <QtQml>
#include <QQmlContext>
#include <QTranslator>
#include <QLocale>

// Re-undefine emit — the Qt headers above re-define it, and any
// Ghostty header included after this point would see the empty macro.
// (This is defensive; ghosttyvt.h handles its own includes correctly.)
#ifdef emit
#undef emit
#endif

static void loadTranslations(QCoreApplication *app)
{
    QLocale locale = QLocale::system();
    auto *translator = new QTranslator(app);
    QString transDir = SailfishApp::pathTo("translations").toLocalFile();
    if (translator->load(locale.name(), "ghosteel", "_", transDir)) {
        app->installTranslator(translator);
    } else {
        // Try language-only (e.g. "de" from "de_DE")
        QString lang = locale.name().left(locale.name().indexOf('_'));
        if (!lang.isEmpty() && translator->load(lang, "ghosteel", "_", transDir)) {
            app->installTranslator(translator);
        } else {
            delete translator;
        }
    }
}

int main(int argc, char *argv[])
{
    qmlRegisterType<TerminalView>(APP_QML_MODULE, 1, 0, "TerminalView");

    QScopedPointer<QGuiApplication> app(SailfishApp::application(argc, argv));
    loadTranslations(app.data());
    QScopedPointer<QQuickView> view(SailfishApp::createView());

    // Expose Settings singleton to QML as a context property
    view->rootContext()->setContextProperty(QStringLiteral("Settings"), Settings::instance());

    // Expose SessionManager singleton to QML
    SessionManager *sessionManager = new SessionManager(app.data());
    view->rootContext()->setContextProperty(QStringLiteral("SessionManager"), sessionManager);

    // Expose version strings to QML
#ifndef GIT_VERSION
#define GIT_VERSION "dev"
#endif
#ifndef GHOSTTY_VERSION
#define GHOSTTY_VERSION "unknown"
#endif
    view->rootContext()->setContextProperty(QStringLiteral("appVersion"), QStringLiteral(GIT_VERSION));
    view->rootContext()->setContextProperty(QStringLiteral("ghosttyVersion"), QStringLiteral(GHOSTTY_VERSION));
    view->rootContext()->setContextProperty(QStringLiteral("appName"), QStringLiteral(APP_NAME));

    view->setSource(SailfishApp::pathToMainQml());
    view->show();

    return app->exec();
}
