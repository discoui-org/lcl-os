#include "lcl-qt/quick_surface.hpp"

#include <QGuiApplication>
#include <QFile>
#include <QMessageLogContext>
#include <QTextStream>

#include <cstdio>
#include <execinfo.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>

namespace {

constexpr const char* kStatusPath = "/Data/qt-smoke-status.log";
int gCrashLogDescriptor = -1;

// The system sandbox discards an application's stderr by design.  Keep this
// integration bundle's diagnostics in its private persistent data directory
// so a QEMU smoke failure remains observable without relaxing that policy.
void recordStatus(const QString& message) {
    QFile file(QString::fromLatin1(kStatusPath));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        return;
    }
    QTextStream(&file) << message << Qt::endl;
}

void qtMessageHandler(QtMsgType type, const QMessageLogContext&,
                      const QString& message) {
    const char* level = "debug";
    switch (type) {
        case QtInfoMsg: level = "info"; break;
        case QtWarningMsg: level = "warning"; break;
        case QtCriticalMsg: level = "critical"; break;
        case QtFatalMsg: level = "fatal"; break;
        case QtDebugMsg: break;
    }
    recordStatus(QStringLiteral("qt[%1]: %2")
                     .arg(QString::fromLatin1(level), message));
    if (type == QtFatalMsg) {
        abort();
    }
}

void fatalSignalHandler(int signalNumber) {
    char message[] = "fatal signal 00\n";
    if (signalNumber < 10) {
        message[13] = ' ';
        message[14] = static_cast<char>('0' + signalNumber);
    } else {
        message[13] = static_cast<char>('0' + ((signalNumber / 10) % 10));
        message[14] = static_cast<char>('0' + (signalNumber % 10));
    }
    if (gCrashLogDescriptor >= 0) {
        const ssize_t ignored = write(gCrashLogDescriptor, message,
                                      sizeof(message) - 1);
        (void)ignored;
        void* frames[48]{};
        const int frameCount = backtrace(frames, 48);
        backtrace_symbols_fd(frames, frameCount, gCrashLogDescriptor);
    }
    _exit(128 + signalNumber);
}

void installCrashDiagnostics() {
    gCrashLogDescriptor = open(kStatusPath,
                               O_WRONLY | O_APPEND | O_CLOEXEC | O_CREAT,
                               0600);
    struct sigaction action {};
    action.sa_handler = fatalSignalHandler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESETHAND;
    for (const int signalNumber : {SIGABRT, SIGBUS, SIGILL, SIGSEGV, SIGSYS}) {
        sigaction(signalNumber, &action, nullptr);
    }
}

} // namespace

int main(int argc, char** argv) {
    constexpr auto kAppId = "org.lcl.qt.smoke";
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }
    // The canonical rootfs keeps Qt's loadable plugins and QML modules outside
    // the flat ELF-library directory. Set both paths before QGuiApplication
    // constructs the QPA integration.
    if (qEnvironmentVariableIsEmpty("QT_PLUGIN_PATH")) {
        qputenv("QT_PLUGIN_PATH", "/System/Library/Qt/plugins");
    }
    if (qEnvironmentVariableIsEmpty("QML2_IMPORT_PATH")) {
        qputenv("QML2_IMPORT_PATH", "/System/Library/Qt/qml");
    }

    bool instanceOk = false;
    const auto instanceId = qEnvironmentVariable("LCL_APP_INSTANCE_ID")
        .toULongLong(&instanceOk);
    if (qEnvironmentVariable("LCL_APP_ID") != QLatin1String(kAppId) ||
        !instanceOk || instanceId == 0) {
        recordStatus(QStringLiteral("rejected direct launch"));
        std::fputs("[lcl-qt] smoke must be launched through lcl-sessiond\n", stderr);
        return 2;
    }
    recordStatus(QStringLiteral("starting instance %1").arg(instanceId));
    installCrashDiagnostics();
    qInstallMessageHandler(qtMessageHandler);
    recordStatus(QStringLiteral("constructing QGuiApplication"));
    QGuiApplication application(argc, argv);
    recordStatus(QStringLiteral("QGuiApplication ready"));

    lcl::client::SurfaceOptions options{};
    options.surfaceId = 1;
    options.appId = kAppId;
    options.appInstanceId = instanceId;
    options.title = "LCL Qt Quick";
    options.bounds = {80.0f, 60.0f, 640.0f, 400.0f};

    recordStatus(QStringLiteral("constructing QuickSurface"));
    lcl::qt::QuickSurface surface;
    recordStatus(QStringLiteral("QuickSurface ready"));
    QObject::connect(&surface, &lcl::qt::QuickSurface::errorOccurred,
                     [](const QString& error) {
                         recordStatus(QStringLiteral("error: %1").arg(error));
                         QTextStream(stderr) << "[lcl-qt] " << error << Qt::endl;
                     });
    QObject::connect(&surface, &lcl::qt::QuickSurface::closeRequested,
                     &application, &QCoreApplication::quit);
    QObject::connect(&surface, &lcl::qt::QuickSurface::framePresented,
                     [](quint64 frameSerial) {
                         recordStatus(QStringLiteral("frame presented %1")
                                          .arg(frameSerial));
                     });
    recordStatus(QStringLiteral("starting QuickSurface"));
    if (!surface.start(options, QUrl(QStringLiteral("qrc:/qml/Main.qml")))) {
        recordStatus(QStringLiteral("QuickSurface start failed"));
        return 1;
    }
    recordStatus(QStringLiteral("QuickSurface started"));
    return application.exec();
}
