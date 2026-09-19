#include "Services/MCP/Transport/HttpTransport.h"

#include <chrono>
#include <random>
#include <sstream>

#include <nlohmann/json.hpp>

namespace gridex::mcp {

namespace {

constexpr qint64 kMaxBodyBytes = 16 * 1024 * 1024;  // 16 MB cap on request bodies

std::string newUuid() {
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    std::uniform_int_distribution<uint64_t> dist;
    uint64_t a = dist(rng), b = dist(rng);
    char buf[40];
    std::snprintf(buf, sizeof(buf),
        "%08x-%04x-%04x-%04x-%012llx",
        static_cast<uint32_t>(a >> 32),
        static_cast<uint16_t>(a >> 16) & 0xFFFF,
        (static_cast<uint16_t>(a) & 0x0FFF) | 0x4000,
        (static_cast<uint16_t>(b >> 48) & 0x3FFF) | 0x8000,
        static_cast<unsigned long long>(b & 0xFFFFFFFFFFFFULL));
    return buf;
}

std::string lower(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

std::string statusText(int code) {
    switch (code) {
        case 200: return "OK";
        case 202: return "Accepted";
        case 204: return "No Content";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 413: return "Payload Too Large";
        case 500: return "Internal Server Error";
        default:  return "Unknown";
    }
}

// Minimal URL decode for query parameter values.
std::string urlDecode(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            const auto hex = s.substr(i + 1, 2);
            try {
                out.push_back(static_cast<char>(std::stoi(hex, nullptr, 16)));
                i += 2;
            } catch (...) {
                out.push_back(s[i]);
            }
        } else if (s[i] == '+') {
            out.push_back(' ');
        } else {
            out.push_back(s[i]);
        }
    }
    return out;
}

}  // namespace

HttpTransport::HttpTransport(quint16 port, Protocol protocol, std::string authToken)
    : port_(port), protocol_(protocol), authToken_(std::move(authToken)) {}

HttpTransport::~HttpTransport() { stop(); }

void HttpTransport::setHandler(RequestHandler h) { handler_ = std::move(h); }

void HttpTransport::start() {
    if (running_) return;
    if (!server_) {
        server_ = new QTcpServer();
        QObject::connect(server_, &QTcpServer::newConnection, [this] { onNewConnection(); });
    }
    if (!server_->listen(QHostAddress::Any, port_)) {
        throw std::runtime_error("HttpTransport: cannot listen on port " + std::to_string(port_));
    }
    running_ = true;
}

void HttpTransport::stop() {
    if (!running_ && !server_) return;
    running_ = false;
    if (server_) {
        server_->close();
        server_->deleteLater();
        server_ = nullptr;
    }
    for (const auto& kv : buffers_) {
        auto* sock = kv.first;
        if (!sseSockets_.count(sock) && sock) {
            sock->disconnectFromHost();
            sock->deleteLater();
        }
    }
    buffers_.clear();
    for (auto* sock : sseSockets_) {
        if (sock) {
            sock->disconnectFromHost();
            sock->deleteLater();
        }
    }
    sseSockets_.clear();
    sseSessions_.clear();
    httpSessions_.clear();
}

void HttpTransport::onNewConnection() {
    while (server_ && server_->hasPendingConnections()) {
        auto* sock = server_->nextPendingConnection();
        if (!sock) break;
        buffers_[sock].clear();
        QObject::connect(sock, &QTcpSocket::readyRead, [this, sock] { onReadyRead(sock); });
        QObject::connect(sock, &QTcpSocket::disconnected, [this, sock] {
            buffers_.erase(sock);
            sseSockets_.erase(sock);
            for (auto it = sseSessions_.begin(); it != sseSessions_.end();) {
                if (it->second == sock) it = sseSessions_.erase(it);
                else ++it;
            }
            sock->deleteLater();
        });
    }
}

void HttpTransport::onReadyRead(QTcpSocket* socket) {
    if (!socket) return;
    auto& buf = buffers_[socket];
    buf.append(socket->readAll());
    processBuffered(socket);
}

void HttpTransport::processBuffered(QTcpSocket* socket) {
    auto& buf = buffers_[socket];
    // Parse as many complete requests as the buffer holds.
    while (true) {
        const int headerEnd = buf.indexOf("\r\n\r\n");
        if (headerEnd < 0) {
            if (buf.size() > 64 * 1024) {  // header flood
                respondSimple(socket, 400, statusText(400), true);
                socket->disconnectFromHost();
                buf.clear();
            }
            return;
        }
        const QByteArray head = buf.left(headerEnd);
        QList<QByteArray> lines = head.split('\n');
        if (lines.isEmpty()) {
            respondSimple(socket, 400, statusText(400), true);
            return;
        }
        QByteArray requestLine = lines[0];
        requestLine = requestLine.trimmed();

        HttpRequest req;
        {
            const QList<QByteArray> parts = requestLine.split(' ');
            if (parts.size() < 3) {
                respondSimple(socket, 400, statusText(400), true);
                return;
            }
            req.method = std::string(parts[0].trimmed().constData());
            req.target = std::string(parts[1].trimmed().constData());
            req.version = std::string(parts[2].trimmed().constData());
        }

        const std::string target = req.target;
        const std::size_t qpos = target.find('?');
        req.path = qpos == std::string::npos ? target : target.substr(0, qpos);
        req.query = qpos == std::string::npos ? "" : target.substr(qpos + 1);

        qint64 contentLength = 0;
        for (int i = 1; i < lines.size(); ++i) {
            const QByteArray line = lines[i].trimmed();
            const int colon = line.indexOf(':');
            if (colon <= 0) continue;
            const std::string key = lower(std::string(line.left(colon).trimmed().constData()));
            const std::string value = std::string(line.mid(colon + 1).trimmed().constData());
            req.headers[key] = value;
            if (key == "content-length") contentLength = std::atoll(value.c_str());
        }

        const std::string connHeader = lower(req.headers.count("connection") ? req.headers["connection"] : "");
        req.keepAlive = req.version != "HTTP/1.0" || connHeader == "keep-alive";
        if (connHeader == "close") req.keepAlive = false;

        if (contentLength < 0 || contentLength > kMaxBodyBytes) {
            respondSimple(socket, 413, statusText(413), true);
            return;
        }
        const qint64 totalNeeded = headerEnd + 4 + contentLength;
        if (buf.size() < totalNeeded) return;  // wait for the rest of the body

        req.body = buf.mid(headerEnd + 4, static_cast<int>(contentLength));
        buf.remove(0, static_cast<int>(totalNeeded));

        const bool keepGoing = dispatch(socket, req);
        if (!keepGoing || !req.keepAlive) {
            socket->disconnectFromHost();
            return;
        }
    }
}

bool HttpTransport::authorized(const HttpRequest& req) const {
    if (authToken_.empty()) return true;
    const auto it = req.headers.find("authorization");
    return it != req.headers.end() && it->second == "Bearer " + authToken_;
}

bool HttpTransport::dispatch(QTcpSocket* socket, const HttpRequest& req) {
    if (!authorized(req)) {
        respondSimple(socket, 401, statusText(401), true);
        return false;
    }

    const bool sseEnabled = protocol_ == Protocol::Sse || protocol_ == Protocol::Both;
    const bool streamableEnabled = protocol_ == Protocol::Streamable || protocol_ == Protocol::Both;

    if (req.path == "/sse" && req.method == "GET" && sseEnabled) return handleSseConnect(socket);
    if (req.path == "/messages" && req.method == "POST" && sseEnabled) return handleMessagesPost(socket, req);

    if (req.path == "/mcp" && streamableEnabled) {
        if (req.method == "POST")  return handleMcpPost(socket, req);
        if (req.method == "GET")   return handleMcpGet(socket, req);
        if (req.method == "DELETE") return handleMcpDelete(socket, req);
        respondSimple(socket, 405, statusText(405), req.keepAlive);
        return true;
    }

    respondSimple(socket, 404, statusText(404), req.keepAlive);
    return true;
}

// ---------- SSE (legacy HTTP+SSE) ----------

bool HttpTransport::handleSseConnect(QTcpSocket* socket) {
    if (!handler_) {
        respondSimple(socket, 500, statusText(500), true);
        return false;
    }
    const std::string sessionId = newSessionId();

    std::ostringstream head;
    head << "HTTP/1.1 200 OK\r\n"
         << "Content-Type: text/event-stream\r\n"
         << "Cache-Control: no-cache\r\n"
         << "Connection: keep-alive\r\n"
         << "\r\n";
    socket->write(QByteArray::fromStdString(head.str()));

    // First event: where to POST JSON-RPC messages for this session.
    const std::string endpointEvent =
        "event: endpoint\ndata: /messages?sessionId=" + sessionId + "\n\n";
    socket->write(QByteArray::fromStdString(endpointEvent));
    socket->flush();

    sseSockets_.insert(socket);
    sseSessions_[sessionId] = socket;
    return true;  // keep the connection open
}

bool HttpTransport::handleMessagesPost(QTcpSocket* socket, const HttpRequest& req) {
    const std::string sessionId = queryParam(req, "sessionId");
    const auto sessionIt = sseSessions_.find(sessionId);
    if (sessionId.empty() || sessionIt == sseSessions_.end()) {
        respondSimple(socket, 404, statusText(404), true);
        return false;
    }

    JSONRPCRequest rpc;
    try {
        const auto j = nlohmann::json::parse(req.body.toStdString());
        rpc = JSONRPCRequest::fromJson(j);
    } catch (const std::exception&) {
        const auto errResp = JSONRPCResponse::err(nlohmann::json(nullptr), JSONRPCError::parseError());
        sendSseMessage(sessionId, errResp.toJson().dump());
        respondSimple(socket, 202, statusText(202), req.keepAlive);
        return true;
    }

    // Responses to notifications must not be produced (JSON-RPC 2.0).
    const bool isNotification = rpc.id.is_null();
    const JSONRPCResponse resp = handler_ ? handler_(rpc)
                                          : JSONRPCResponse::err(rpc.id, JSONRPCError::internalError());
    if (!isNotification) sendSseMessage(sessionId, resp.toJson().dump());

    respondSimple(socket, 202, statusText(202), req.keepAlive);
    return true;
}

void HttpTransport::sendSseMessage(const std::string& sessionId, const std::string& jsonPayload) {
    const auto it = sseSessions_.find(sessionId);
    if (it == sseSessions_.end()) return;
    if (auto* sock = it->second; sock && sock->state() == QAbstractSocket::ConnectedState) {
        const std::string frame = "event: message\ndata: " + jsonPayload + "\n\n";
        sock->write(QByteArray::fromStdString(frame));
        sock->flush();
    }
}

void HttpTransport::closeSseSession(const std::string& sessionId) {
    const auto it = sseSessions_.find(sessionId);
    if (it == sseSessions_.end()) return;
    if (auto* sock = it->second) {
        sseSockets_.erase(sock);
        sock->disconnectFromHost();
    }
    sseSessions_.erase(it);
}

// ---------- Streamable HTTP ----------

bool HttpTransport::handleMcpPost(QTcpSocket* socket, const HttpRequest& req) {
    if (!handler_) {
        respondSimple(socket, 500, statusText(500), true);
        return false;
    }

    nlohmann::json parsed;
    try {
        parsed = nlohmann::json::parse(req.body.toStdString());
    } catch (const std::exception&) {
        writeJsonResponse(socket, 400, statusText(400),
                          JSONRPCResponse::err(nlohmann::json(nullptr), JSONRPCError::parseError()),
                          "", req.keepAlive);
        return true;
    }

    std::string sessionId;
    if (const auto sit = req.headers.find("mcp-session-id"); sit != req.headers.end()) sessionId = sit->second;

    // Batched JSON-RPC (array) — process each entry, return the responses.
    if (parsed.is_array()) {
        nlohmann::json responses = nlohmann::json::array();
        for (const auto& entry : parsed) {
            const JSONRPCRequest rpc = JSONRPCRequest::fromJson(entry);
            const bool isNotification = rpc.id.is_null();
            JSONRPCResponse resp = handler_(rpc);
            if (!isNotification) responses.push_back(resp.toJson());
        }
        if (responses.empty()) {
            respondSimple(socket, 202, statusText(202), req.keepAlive);
            return true;
        }
        std::map<std::string, std::string> headers{{"Content-Type", "application/json"}};
        if (!sessionId.empty()) headers["Mcp-Session-Id"] = sessionId;
        writeHttpResponse(socket, 200, statusText(200), headers,
                          QByteArray::fromStdString(responses.dump()), req.keepAlive);
        return true;
    }

    const JSONRPCRequest rpc = JSONRPCRequest::fromJson(parsed);
    const bool isNotification = rpc.id.is_null();

    // initialize establishes the session; unknown session ids are rejected.
    const bool isInitialize = rpc.method == "initialize";
    if (!sessionId.empty() && !isInitialize && !httpSessions_.count(sessionId)) {
        writeHttpResponse(socket, 404, statusText(404),
                          {{"Content-Type", "application/json"}},
                          QByteArray::fromStdString(
                              JSONRPCResponse::err(rpc.id, JSONRPCError::invalidRequest()).toJson().dump()),
                          true);
        return false;
    }

    JSONRPCResponse resp = handler_(rpc);

    if (isNotification) {
        respondSimple(socket, 202, statusText(202), req.keepAlive);
        return true;
    }

    if (isInitialize) {
        sessionId = newSessionId();
        httpSessions_.insert(sessionId);
    }

    writeJsonResponse(socket, 200, statusText(200), resp, sessionId, req.keepAlive);
    return true;
}

bool HttpTransport::handleMcpGet(QTcpSocket* socket, const HttpRequest& req) {
    // Server-initiated SSE streams are not offered; per spec 405 is allowed.
    std::map<std::string, std::string> headers{{"Allow", "POST, DELETE"}};
    writeHttpResponse(socket, 405, statusText(405), headers, QByteArray(), req.keepAlive);
    return true;
}

bool HttpTransport::handleMcpDelete(QTcpSocket* socket, const HttpRequest& req) {
    if (const auto sit = req.headers.find("mcp-session-id"); sit != req.headers.end()) {
        httpSessions_.erase(sit->second);
    }
    respondSimple(socket, 200, statusText(200), req.keepAlive);
    return true;
}

// ---------- response writers ----------

void HttpTransport::writeHttpResponse(QTcpSocket* socket, int status, const std::string& statusTextStr,
                                      const std::map<std::string, std::string>& extraHeaders,
                                      const QByteArray& body, bool close) {
    std::ostringstream head;
    head << "HTTP/1.1 " << status << " " << statusTextStr << "\r\n";
    head << "Content-Length: " << body.size() << "\r\n";
    head << "Connection: " << (close ? "close" : "keep-alive") << "\r\n";
    for (const auto& [k, v] : extraHeaders) head << k << ": " << v << "\r\n";
    head << "\r\n";
    socket->write(QByteArray::fromStdString(head.str()));
    if (!body.isEmpty()) socket->write(body);
    socket->flush();
}

void HttpTransport::writeJsonResponse(QTcpSocket* socket, int status, const std::string& statusTextStr,
                                      const JSONRPCResponse& resp, const std::string& sessionId,
                                      bool close) {
    std::map<std::string, std::string> headers{{"Content-Type", "application/json"}};
    if (!sessionId.empty()) headers["Mcp-Session-Id"] = sessionId;
    writeHttpResponse(socket, status, statusTextStr, headers,
                      QByteArray::fromStdString(resp.toJson().dump()), close);
}

void HttpTransport::respondSimple(QTcpSocket* socket, int status, const std::string& statusTextStr,
                                  bool close) {
    writeHttpResponse(socket, status, statusTextStr, {}, QByteArray(), close);
}

std::string HttpTransport::queryParam(const HttpRequest& req, const std::string& key) const {
    std::istringstream stream(req.query);
    std::string pair;
    while (std::getline(stream, pair, '&')) {
        const std::size_t eq = pair.find('=');
        if (eq == std::string::npos) continue;
        if (pair.substr(0, eq) == key) return urlDecode(pair.substr(eq + 1));
    }
    return "";
}

std::string HttpTransport::newSessionId() const {
    return newUuid();
}

}  // namespace gridex::mcp
