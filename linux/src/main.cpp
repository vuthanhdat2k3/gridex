#include <QApplication>
#include <QSettings>
#include <QStringList>
#include <QStyleFactory>
#include <QThread>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <memory>

#include "App/Headless/ConnectionSeeder.h"
#include "App/MCP/MCPConnectionProvider.h"
#include "Data/Keychain/SecretStore.h"
#include "Data/Persistence/AppConnectionRepository.h"
#include "Data/Persistence/AppDatabase.h"
#include "Presentation/Theme/ThemeManager.h"
#include "Presentation/Views/ExplainVisualizer/ExplainExtension.h"
#include "Presentation/Windows/Main/MainWindow.h"
#include "Services/MCP/MCPServer.h"
#include "Services/MCP/Transport/HttpTransport.h"

namespace {

constexpr const char* kDefaultMcpHttpPort = "8102";

void ensureHeadlessQtPlatform() {
    // Containers have no display server; the offscreen platform plugin keeps
    // QApplication (needed for QSettings/threads/network) happy without X11.
    if (!qEnvironmentVariableIsSet("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }
}

// Applies --mcp-seed-connections <file> / GRIDEX_SEED_CONNECTIONS. Idempotent:
// connections are upserted by id; MCP modes are persisted to QSettings.
void seedHeadlessConnections(gridex::AppConnectionRepository& repo,
                             gridex::SecretStore& secretStore,
                             const QString& seedPath) {
    if (seedPath.isEmpty()) return;
    try {
        const auto seeded = gridex::ConnectionSeeder::seedFromFile(
            seedPath.toStdString(), repo, secretStore);
        QSettings s;
        for (const auto& sc : seeded) {
            if (sc.mode) {
                s.setValue(QStringLiteral("mcp.connectionMode.") + QString::fromStdString(sc.id),
                           QString::fromStdString(std::string(gridex::rawValue(*sc.mode))));
            }
        }
        std::fprintf(stderr, "Gridex: seeded %zu connection(s) from %s\n",
                     seeded.size(), seedPath.toStdString().c_str());
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Gridex: connection seeding failed: %s\n", e.what());
        throw;
    }
}

QString resolveSeedPath(const QStringList& args) {
    const int idx = args.indexOf(QStringLiteral("--mcp-seed-connections"));
    if (idx >= 0 && idx + 1 < args.size()) return args[idx + 1];
    const QByteArray env = qgetenv("GRIDEX_SEED_CONNECTIONS");
    return QString::fromUtf8(env);
}

void seedConnectionModesFromSettings(gridex::mcp::MCPServer& server,
                                     gridex::AppConnectionRepository& repo) {
    QSettings s;
    for (const auto& c : repo.fetchAll()) {
        QString id = QString::fromStdString(c.id);
        QString raw = s.value(QStringLiteral("mcp.connectionMode.") + id).toString();
        auto m = gridex::mcpConnectionModeFromRaw(raw.toStdString());
        if (m) server.setConnectionMode(c.id, *m);
    }
}

void applyRateLimitsFromSettings(gridex::mcp::MCPServer& server) {
    QSettings s;
    gridex::mcp::RateLimits rl;
    rl.queriesPerMinute = s.value("mcp.rateLimit.queriesPerMinute", 60).toInt();
    rl.queriesPerHour   = s.value("mcp.rateLimit.queriesPerHour",   1000).toInt();
    rl.writesPerMinute  = s.value("mcp.rateLimit.writesPerMinute",  10).toInt();
    rl.ddlPerMinute     = s.value("mcp.rateLimit.ddlPerMinute",     1).toInt();
    server.rateLimiter().setLimits(rl);
}

int runMcpStdio(int argc, char* argv[]) {
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("Gridex"));
    QApplication::setOrganizationName(QStringLiteral("Vurakit"));
    QApplication::setOrganizationDomain(QStringLiteral("vurakit.com"));

    auto db = std::make_shared<gridex::AppDatabase>();
    db->open();

    auto repo        = std::make_unique<gridex::AppConnectionRepository>(db);
    auto secretStore = std::make_unique<gridex::SecretStore>();
    auto provider    = std::make_unique<gridex::MCPConnectionProvider>(repo.get(), secretStore.get());

    QStringList args;
    for (int i = 1; i < argc; ++i) args << QString::fromUtf8(argv[i]);
    seedHeadlessConnections(*repo, *secretStore, resolveSeedPath(args));

    std::shared_ptr<gridex::mcp::IMCPConnectionProvider> shim(provider.get(), [](auto*){});
    auto server = std::make_unique<gridex::mcp::MCPServer>(
        shim, "1.0.0", gridex::mcp::MCPTransportMode::Stdio);

    seedConnectionModesFromSettings(*server, *repo);
    applyRateLimitsFromSettings(*server);

    server->start();
    // Background watcher quits Qt when stdin EOF causes the transport to stop.
    QThread* watcher = QThread::create([&]() {
        while (server->isRunning()) QThread::msleep(250);
        QMetaObject::invokeMethod(qApp, "quit", Qt::QueuedConnection);
    });
    watcher->start();
    int code = QApplication::exec();
    server->stop();
    watcher->wait(1000);
    return code;
}

int runMcpHttp(int argc, char* argv[], gridex::mcp::HttpTransport::Protocol protocol,
               quint16 port, const std::string& authToken) {
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("Gridex"));
    QApplication::setOrganizationName(QStringLiteral("Vurakit"));
    QApplication::setOrganizationDomain(QStringLiteral("vurakit.com"));

    auto db = std::make_shared<gridex::AppDatabase>();
    db->open();

    auto repo        = std::make_unique<gridex::AppConnectionRepository>(db);
    auto secretStore = std::make_unique<gridex::SecretStore>();
    auto provider    = std::make_unique<gridex::MCPConnectionProvider>(repo.get(), secretStore.get());

    QStringList args;
    for (int i = 1; i < argc; ++i) args << QString::fromUtf8(argv[i]);
    seedHeadlessConnections(*repo, *secretStore, resolveSeedPath(args));

    std::shared_ptr<gridex::mcp::IMCPConnectionProvider> shim(provider.get(), [](auto*){});
    // HttpOnly mode: no stdio transport; handleRequest() is driven by HttpTransport.
    auto server = std::make_unique<gridex::mcp::MCPServer>(
        shim, "1.0.0", gridex::mcp::MCPTransportMode::HttpOnly);

    seedConnectionModesFromSettings(*server, *repo);
    applyRateLimitsFromSettings(*server);

    gridex::mcp::HttpTransport http(port, protocol, authToken);
    http.setHandler([&server](const gridex::mcp::JSONRPCRequest& req) {
        return server->handleRequest(req);
    });

    server->start();
    http.start();
    std::fprintf(stderr, "Gridex MCP HTTP server listening on port %u (protocol: %s)\n",
                 static_cast<unsigned>(port),
                 protocol == gridex::mcp::HttpTransport::Protocol::Sse ? "sse"
                     : protocol == gridex::mcp::HttpTransport::Protocol::Streamable ? "streamable"
                     : "both");

    const int code = QApplication::exec();
    http.stop();
    server->stop();
    return code;
}

}  // namespace

int main(int argc, char* argv[]) {
    try {
        QStringList args;
        for (int i = 1; i < argc; ++i) args << QString::fromUtf8(argv[i]);

        if (args.contains(QStringLiteral("--mcp-stdio"))) {
            ensureHeadlessQtPlatform();
            return runMcpStdio(argc, argv);
        }

        if (args.contains(QStringLiteral("--mcp-http"))) {
            ensureHeadlessQtPlatform();

            quint16 port = 0;
            const int portIdx = args.indexOf(QStringLiteral("--port"));
            if (portIdx >= 0 && portIdx + 1 < args.size()) {
                port = static_cast<quint16>(args[portIdx + 1].toUShort());
            }
            if (port == 0) {
                port = static_cast<quint16>(
                    qEnvironmentVariable("GRIDEX_MCP_PORT", kDefaultMcpHttpPort).toUShort());
            }

            gridex::mcp::HttpTransport::Protocol protocol = gridex::mcp::HttpTransport::Protocol::Both;
            const int protoIdx = args.indexOf(QStringLiteral("--http-transport"));
            QString protoRaw = protoIdx >= 0 && protoIdx + 1 < args.size()
                                   ? args[protoIdx + 1]
                                   : qEnvironmentVariable("GRIDEX_MCP_TRANSPORT");
            protoRaw = protoRaw.toLower();
            if (protoRaw == QStringLiteral("sse")) {
                protocol = gridex::mcp::HttpTransport::Protocol::Sse;
            } else if (protoRaw == QStringLiteral("streamable") || protoRaw == QStringLiteral("http")) {
                protocol = gridex::mcp::HttpTransport::Protocol::Streamable;
            } else if (protoRaw.isEmpty() || protoRaw == QStringLiteral("both")) {
                protocol = gridex::mcp::HttpTransport::Protocol::Both;
            } else {
                std::fprintf(stderr, "FATAL: unknown --http-transport '%s' (sse|streamable|both)\n",
                             protoRaw.toStdString().c_str());
                return 2;
            }

            std::string authToken;
            if (const char* token = std::getenv("GRIDEX_MCP_TOKEN"); token && *token) {
                authToken = token;
            }

            return runMcpHttp(argc, argv, protocol, port, authToken);
        }

        QApplication app(argc, argv);

        QApplication::setApplicationName(QStringLiteral("Gridex"));
        QApplication::setOrganizationName(QStringLiteral("Vurakit"));
        QApplication::setOrganizationDomain(QStringLiteral("vurakit.com"));

        if (auto* fusion = QStyleFactory::create(QStringLiteral("Fusion"))) {
            QApplication::setStyle(fusion);
        }

        gridex::ThemeManager::instance().apply(&app);

        gridex::explain::registerQueryEditorExtension();

        gridex::MainWindow window;
        window.show();

        return QApplication::exec();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FATAL: %s\n", e.what());
        return 1;
    } catch (...) {
        std::fprintf(stderr, "FATAL: unknown exception\n");
        return 1;
    }
}
