#pragma once

#include <functional>
#include <map>
#include <set>
#include <string>

#include <QByteArray>
#include <QObject>
#include <QTcpServer>
#include <QTcpSocket>

#include "Services/MCP/Protocol.h"

namespace gridex::mcp {

// Network MCP transport for headless deployments (containers, servers).
//
// Hosts two wire protocols on one TCP port, selected per endpoint:
//
//   Legacy HTTP+SSE (MCP 2024-11-05):
//     GET  /sse                        → open event stream; first event is
//                                      "endpoint" carrying the POST URI with
//                                      the session id.
//     POST /messages?sessionId=<sid>   → JSON-RPC request; response is written
//                                      to the SSE stream ("message" event),
//                                      HTTP reply is 202 Accepted.
//
//   Streamable HTTP (MCP 2025-03-26):
//     POST /mcp                        → JSON-RPC (single or batch); response
//                                      is application/json. Initialize creates
//                                      a session returned via Mcp-Session-Id.
//     GET  /mcp                        → 405 (no server-initiated stream).
//     DELETE /mcp                      → end session.
//
// When authToken is non-empty every request must carry
// "Authorization: Bearer <token>" or it is rejected with 401.
//
// All handling runs on the thread owning this object (the Qt main thread);
// tool execution is synchronous, matching the stdio transport's semantics.
// Without a GUI dialog wired up the ApprovalGate denies tier-3 write tools
// (deny-by-default), so HTTP deployments are effectively read-only unless
// connection modes are explicitly set to read_write AND approvals are wired.
class HttpTransport {
public:
    enum class Protocol {
        Sse,         // legacy /sse + /messages only
        Streamable,  // /mcp only
        Both,
    };

    using RequestHandler = std::function<JSONRPCResponse(const JSONRPCRequest&)>;

    HttpTransport(quint16 port, Protocol protocol, std::string authToken);
    ~HttpTransport();

    void setHandler(RequestHandler h);

    void start();  // begins listening; must be called from the Qt event-loop thread
    void stop();

    [[nodiscard]] bool isRunning() const noexcept { return running_; }
    [[nodiscard]] quint16 port() const noexcept { return port_; }

private:
    struct HttpRequest {
        std::string method;
        std::string target;                                     // raw path?query
        std::string path;                                       // path without query
        std::string query;                                      // raw query string (no '?')
        std::map<std::string, std::string> headers;             // lower-cased keys
        QByteArray body;
        bool keepAlive = true;
    };

    void onNewConnection();
    void onSocketDisconnected();
    void onReadyRead(QTcpSocket* socket);
    void processBuffered(QTcpSocket* socket);
    bool dispatch(QTcpSocket* socket, const HttpRequest& req);

    // endpoint handlers — return false when the connection must close
    bool handleSseConnect(QTcpSocket* socket);
    bool handleMessagesPost(QTcpSocket* socket, const HttpRequest& req);
    bool handleMcpPost(QTcpSocket* socket, const HttpRequest& req);
    bool handleMcpGet(QTcpSocket* socket, const HttpRequest& req);
    bool handleMcpDelete(QTcpSocket* socket, const HttpRequest& req);

    void writeHttpResponse(QTcpSocket* socket, int status, const std::string& statusText,
                           const std::map<std::string, std::string>& extraHeaders,
                           const QByteArray& body, bool close);
    void writeJsonResponse(QTcpSocket* socket, int status, const std::string& statusText,
                           const JSONRPCResponse& resp, const std::string& sessionId,
                           bool close);
    void respondSimple(QTcpSocket* socket, int status, const std::string& statusText,
                       bool close);
    void sendSseMessage(const std::string& sessionId, const std::string& jsonPayload);
    void closeSseSession(const std::string& sessionId);

    [[nodiscard]] bool authorized(const HttpRequest& req) const;
    [[nodiscard]] std::string newSessionId() const;
    [[nodiscard]] std::string queryParam(const HttpRequest& req, const std::string& key) const;

    QTcpServer* server_ = nullptr;
    quint16 port_;
    Protocol protocol_;
    std::string authToken_;
    RequestHandler handler_;

    std::map<QTcpSocket*, QByteArray> buffers_;
    std::map<std::string, QTcpSocket*> sseSessions_;   // sessionId → stream socket
    std::set<QTcpSocket*> sseSockets_;                 // sockets owned by SSE sessions
    std::set<std::string> httpSessions_;               // streamable-HTTP session ids
    bool running_ = false;
};

}  // namespace gridex::mcp
