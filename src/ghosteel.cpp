// Include terminalview.h BEFORE Qt headers to ensure Ghostty's C headers
// are parsed before Qt defines the `emit` macro (which conflicts with
// Ghostty's use of 'emit' as a struct field name).
#include "terminalview.h"
#include "glrenderer.h"
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
#include <QDBusConnection>
#include <QDBusError>

// Re-undefine emit — the Qt headers above re-define it, and any
// Ghostty header included after this point would see the empty macro.
// (This is defensive; ghosttyvt.h handles its own includes correctly.)
#ifdef emit
#undef emit
#endif

#include "ghosteeladapter.h"
#include "kittyimagedecoder.h"

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
    qmlRegisterType<GLRenderer>(APP_QML_MODULE, 1, 0, "GLRenderer");

    // Register PNG decoder for Kitty Graphics Protocol (process-global, once)
    kittyImageDecoderRegister();

    // Single-instance guard: if another instance is running, tell it to raise
    // its window and exit.  This handles D-Bus activation launching a duplicate
    // when a sandboxed instance is already running.
    if (SessionManager::checkSingleInstance())
        return 0;

    QScopedPointer<QGuiApplication> app(SailfishApp::application(argc, argv));
    loadTranslations(app.data());
    QScopedPointer<QQuickView> view(SailfishApp::createView());

    // Expose Settings singleton to QML as a context property
    view->rootContext()->setContextProperty(QStringLiteral("Settings"), Settings::instance());

    // Expose SessionManager singleton to QML
    SessionManager *sessionManager = new SessionManager(app.data());
    view->rootContext()->setContextProperty(QStringLiteral("SessionManager"), sessionManager);

    // Register D-Bus adaptor for notification action callbacks.
    // Under Sailjail, D-Bus name ownership is restricted, so registration
    // may fail silently — this is expected and notification actions will
    // simply not work when sandboxed.  When launched via D-Bus activation
    // (the .service file), registration succeeds and actions work.
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (bus.isConnected()) {
        if (bus.registerService(QStringLiteral("com.zackslash.ghosteel"))) {
            // Create adaptor as child of sessionManager — must exist before registerObject
            new GhosteelAdapter(sessionManager);
            if (bus.registerObject(QStringLiteral("/com/zackslash/ghosteel"), sessionManager)) {
                qDebug() << "Ghosteel: D-Bus service registered";
                sessionManager->setDbusRegistered(true);
            } else {
                qWarning() << "Ghosteel: D-Bus object registration failed:" << bus.lastError().message();
                bus.unregisterService(QStringLiteral("com.zackslash.ghosteel"));
            }
        } else {
            // Expected under Sailjail — dbus-user.own is restricted
            qDebug() << "Ghosteel: D-Bus service not registered (expected under Sailjail):" << bus.lastError().message();
        }
    } else {
        qDebug() << "Ghosteel: D-Bus session bus not available";
    }

    // Start single-instance socket server so future D-Bus activations
    // can detect this instance instead of spawning a duplicate.
    sessionManager->startSingleInstanceServer();

    // Expose version strings to QML (always defined via -D flags from ghosteel.pro)
    view->rootContext()->setContextProperty(QStringLiteral("appVersion"), QStringLiteral(GIT_VERSION));
    view->rootContext()->setContextProperty(QStringLiteral("ghosttyVersion"), QStringLiteral(GHOSTTY_VERSION));
    view->rootContext()->setContextProperty(QStringLiteral("appName"), QStringLiteral(APP_NAME));

    view->setSource(SailfishApp::pathToMainQml());
    view->show();

    return app->exec();
}
