// Slow Mail: Qt Quick front end for the reMarkable Paper Pro (also runs on a desktop host).
#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QGuiApplication>
#include <QLoggingCategory>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QTimer>
#include <QUrl>

#include <cstdio>
#include <functional>
#include <memory>

#include "app_context.hpp"
#include "triage_controller.hpp"

using slowmail::ui::TriageController;

namespace {

// Qt (and QML console.log) can echo arbitrary strings, which could include mail content. Keep warnings and
// errors but drop all debug/info output from every category.
void quietCategories(QLoggingCategory* cat) {
    cat->setEnabled(QtDebugMsg, false);
    cat->setEnabled(QtInfoMsg, false);
}

void pump(int ms) {
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

bool waitUntil(const std::function<bool()>& cond, int timeoutMs) {
    int waited = 0;
    while (!cond() && waited < timeoutMs) {
        pump(50);
        waited += 50;
    }
    return cond();
}

bool grab(QQuickWindow* win, const QString& dir, const QString& name) {
    pump(250);   // let layout, model updates and polish settle; there are no animations to wait for
    const QImage img = win->grabWindow();
    const QString path = QDir(dir).filePath(name);
    if (img.isNull() || !img.save(path)) {
        std::fprintf(stderr, "screenshot failed: %s\n", qPrintable(path));
        return false;
    }
    std::printf("screenshot: %s (%dx%d)\n", qPrintable(path), img.width(), img.height());
    return true;
}

int runScreenshots(QQuickWindow* win, QObject* root, TriageController& triage, const QString& dir) {
    QDir().mkpath(dir);
    int failures = 0;
    waitUntil([&]() { return triage.ready(); }, 10000);
    pump(300);

    QMetaObject::invokeMethod(root, "showQueue");
    if (!grab(win, dir, QStringLiteral("01-queue.png"))) ++failures;

    auto* model = qobject_cast<slowmail::ui::RowModel*>(triage.queueModel());
    if (model && model->count() > 0) {
        bool done = false;
        auto c1 = QObject::connect(&triage, &TriageController::messageOpened, [&](const QString&, const QString&, const QString&, bool) { done = true; });
        auto c2 = QObject::connect(&triage, &TriageController::messageOpenFailed, [&](const QString&, const QString&) { done = true; });
        QMetaObject::invokeMethod(root, "showMessage", Q_ARG(QVariant, 0));
        waitUntil([&]() { return done; }, 8000);
        QObject::disconnect(c1);
        QObject::disconnect(c2);
        if (!grab(win, dir, QStringLiteral("02-message.png"))) ++failures;
        QMetaObject::invokeMethod(root, "showQueue");
    } else {
        std::printf("no queue items: message screenshot skipped\n");
    }

    QMetaObject::invokeMethod(root, "showActivity");
    if (!grab(win, dir, QStringLiteral("03-activity.png"))) ++failures;

    QMetaObject::invokeMethod(root, "showSignIn");
    if (!grab(win, dir, QStringLiteral("04-signin.png"))) ++failures;
    return failures;
}

} // namespace

int main(int argc, char* argv[]) {
    bool simulator = false;
    bool demo = false;
    QString screenshotDir;
    for (int i = 1; i < argc; ++i) {
        const QByteArray a(argv[i]);
        if (a == "--simulator") simulator = true;
        else if (a == "--demo") demo = true;
        else if (a == "--screenshot-dir" && i + 1 < argc) screenshotDir = QString::fromLocal8Bit(argv[++i]);
        else if (a == "--help" || a == "-h") {
            std::puts("Usage: slowmail [--simulator] [--demo] [--screenshot-dir <dir>]\n"
                      "  --simulator            print the environment configuration in use\n"
                      "  --demo                 use canned sample data instead of a session (UI review only)\n"
                      "  --screenshot-dir DIR   render Queue, Message, Activity and SignIn pages to PNG and exit");
            return 0;
        }
    }

    if (qEnvironmentVariableIsEmpty("QT_QUICK_CONTROLS_STYLE")) qputenv("QT_QUICK_CONTROLS_STYLE", "Basic");
    // Headless rendering (offscreen QPA, screenshots) has no GPU: use the software scene graph unless told otherwise.
    const bool headless = qgetenv("QT_QPA_PLATFORM") == "offscreen" || !screenshotDir.isEmpty();
    if (headless && qEnvironmentVariableIsEmpty("QT_QUICK_BACKEND") && qEnvironmentVariableIsEmpty("QSG_RHI_BACKEND")) {
        QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);
    }

    QLoggingCategory::installFilter(quietCategories);
    QGuiApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("Slow Mail"));

    std::unique_ptr<slowmail::app::AppContext> ctx;
    QString configError;
    const slowmail::app::AppConfig cfg = slowmail::app::AppConfig::from_environment();
    if (simulator) std::fputs(cfg.describe().c_str(), stdout);
    if (!demo) {
        std::string err;
        ctx = slowmail::app::AppContext::create(cfg, err);
        if (!ctx) configError = QString::fromStdString(err);
    }

    TriageController triage(ctx.get());
    if (demo) triage.loadDemoData();
    else if (!ctx) triage.showConfigError(configError);

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("triage"), &triage);
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed, &app, []() { QCoreApplication::exit(1); },
                     Qt::QueuedConnection);
    engine.load(QUrl(QStringLiteral("qrc:/SlowMail/qml/Main.qml")));
    if (engine.rootObjects().isEmpty()) return 1;

    QObject::connect(&app, &QCoreApplication::aboutToQuit, &triage, [&triage]() { triage.shutdown(); });
    if (ctx) triage.startSession();

    if (!screenshotDir.isEmpty()) {
        QObject* root = engine.rootObjects().first();
        auto* win = qobject_cast<QQuickWindow*>(root);
        if (!win) return 1;
        int failures = 0;
        QTimer::singleShot(0, &app, [&]() {
            failures = runScreenshots(win, root, triage, screenshotDir);
            QCoreApplication::exit(failures == 0 ? 0 : 1);
        });
        return app.exec();
    }
    return app.exec();
}
