#include "McpServer.h"

#include <chrono>
#include <cstdio>
#include <random>
#include <utility>

namespace
{
    // 支持的 MCP 协议版本与最新版本。初始化握手时:
    // 客户端请求的版本受支持则原样返回,否则返回最新版本。
    const char* const kSupportedVersions[] = {
        "2024-11-05",
        "2025-03-26",
        "2025-06-18",
        "2025-11-25"
    };
    const char* const kLatestProtocolVersion = "2025-11-25";

    // SSE 心跳间隔。SSE 注释行(以 ':' 开头)对客户端不可见,仅保活。
    constexpr auto kSseHeartbeatInterval = std::chrono::seconds(15);

    std::string TrimCopy(const std::string& text)
    {
        const auto isSpace = [](unsigned char ch)
        {
            return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n' || ch == '\f' || ch == '\v';
        };

        std::size_t begin = 0;
        while (begin < text.size() && isSpace(static_cast<unsigned char>(text[begin])))
        {
            ++begin;
        }

        std::size_t end = text.size();
        while (end > begin && isSpace(static_cast<unsigned char>(text[end - 1])))
        {
            --end;
        }

        return text.substr(begin, end - begin);
    }
}

McpServer::~McpServer()
{
    Shutdown();
}

void McpServer::SetServerInfo(std::string name, std::string title, std::string version)
{
    serverName_ = std::move(name);
    serverTitle_ = std::move(title);
    serverVersion_ = std::move(version);
}

void McpServer::SetInstructions(std::string instructions)
{
    instructions_ = std::move(instructions);
}

void McpServer::AddTool(Tool tool)
{
    tools_.push_back(std::move(tool));
}

void McpServer::RegisterRoutes(httplib::Server& server)
{
    server.Post("/mcp",
        [this](const httplib::Request& request, httplib::Response& response)
        {
            HandlePost(request, response);
        });

    server.Get("/mcp",
        [this](const httplib::Request& request, httplib::Response& response)
        {
            HandleGet(request, response);
        });

    server.Delete("/mcp",
        [this](const httplib::Request& request, httplib::Response& response)
        {
            HandleDelete(request, response);
        });
}

void McpServer::Shutdown()
{
    std::vector<SessionPtr> allSessions;

    {
        std::lock_guard<std::mutex> lock(sessionsMutex_);
        allSessions.reserve(sessions_.size());
        for (auto& entry : sessions_)
        {
            allSessions.push_back(entry.second);
        }
        sessions_.clear();
    }

    for (auto& session : allSessions)
    {
        {
            std::lock_guard<std::mutex> lock(session->streamMutex);
            session->closed = true;
        }
        session->streamCv.notify_all();
    }
}

void McpServer::HandlePost(
    const httplib::Request& request,
    httplib::Response& response)
{
    // 规范要求:Origin 存在且不合法时必须返回 403(防 DNS rebinding)。
    if (!IsOriginAllowed(request.get_header_value("Origin")))
    {
        response.status = 403;
        return;
    }

    json body;

    try
    {
        body = json::parse(request.body);
    }
    catch (const json::exception&)
    {
        SetJsonBody(response, 400, BuildError(json(), -32700, "Parse error"));
        return;
    }

    SessionPtr session;
    const std::string sessionId = request.get_header_value("Mcp-Session-Id");

    if (!sessionId.empty())
    {
        session = FindSession(sessionId);

        if (!session)
        {
            // 会话已被服务端终止或无效:客户端必须重新 initialize。
            response.status = 404;
            return;
        }
    }

    const std::string versionHeader =
        request.get_header_value("MCP-Protocol-Version");

    if (!versionHeader.empty() && !IsSupportedVersion(versionHeader))
    {
        SetJsonBody(
            response,
            400,
            BuildError(json(), -32600, "Invalid Request: unsupported MCP-Protocol-Version"));
        return;
    }

    if (body.is_array())
    {
        HandleBatchPost(session, body, response);
        return;
    }

    if (!body.is_object())
    {
        SetJsonBody(response, 400, BuildError(json(), -32600, "Invalid Request"));
        return;
    }

    const bool hasMethod =
        body.contains("method") && body["method"].is_string();
    const bool hasId = body.contains("id") && !body["id"].is_null();
    const bool isInitializeRequest =
        hasMethod && hasId && body["method"].get<std::string>() == "initialize";

    // 除 initialize 外,请求必须携带有效会话。
    if (!session && !isInitializeRequest && hasMethod && hasId)
    {
        SetJsonBody(
            response,
            400,
            BuildError(
                body["id"],
                -32600,
                "Invalid Request: Mcp-Session-Id header is required"));
        return;
    }

    bool sessionCreated = false;
    json reply;

    try
    {
        reply = ProcessMessage(session, body, sessionCreated);
    }
    catch (const std::exception&)
    {
        SetJsonBody(response, 500, BuildError(json(), -32603, "Internal error"));
        return;
    }

    if (sessionCreated && session)
    {
        response.set_header("Mcp-Session-Id", session->id);
    }

    if (reply.is_null())
    {
        // 通知或客户端发来的响应消息:202 Accepted,无响应体。
        response.status = 202;
        return;
    }

    SetJsonBody(response, 200, reply);
}

void McpServer::HandleBatchPost(
    SessionPtr& session,
    const json& body,
    httplib::Response& response)
{
    // 自 2025-06-18 版本起,协议移除了 JSON-RPC 批量消息。
    if (session && !BatchingAllowed(session->protocolVersion))
    {
        SetJsonBody(
            response,
            400,
            BuildError(json(), -32600, "Invalid Request: JSON-RPC batching is not supported"));
        return;
    }

    if (!session)
    {
        // 会话尚未建立:仅当批量中包含 initialize 时才处理(旧版本协议允许)。
        bool containsInitialize = false;

        for (const auto& message : body)
        {
            if (message.is_object() &&
                message.contains("method") &&
                message["method"] == "initialize" &&
                message.contains("id"))
            {
                containsInitialize = true;
                break;
            }
        }

        if (!containsInitialize)
        {
            SetJsonBody(
                response,
                400,
                BuildError(json(), -32600, "Invalid Request: Mcp-Session-Id header is required"));
            return;
        }
    }

    std::vector<json> replies;
    bool sessionCreated = false;

    try
    {
        for (const auto& message : body)
        {
            auto reply = ProcessMessage(session, message, sessionCreated);

            if (!reply.is_null())
            {
                replies.push_back(std::move(reply));
            }
        }
    }
    catch (const std::exception&)
    {
        SetJsonBody(response, 500, BuildError(json(), -32603, "Internal error"));
        return;
    }

    if (sessionCreated && session)
    {
        response.set_header("Mcp-Session-Id", session->id);
    }

    if (replies.empty())
    {
        response.status = 202;
        return;
    }

    json batch = json::array();

    for (auto& reply : replies)
    {
        batch.push_back(std::move(reply));
    }

    SetJsonBody(response, 200, batch);
}

void McpServer::HandleGet(
    const httplib::Request& request,
    httplib::Response& response)
{
    if (!IsOriginAllowed(request.get_header_value("Origin")))
    {
        response.status = 403;
        return;
    }

    const std::string sessionId = request.get_header_value("Mcp-Session-Id");

    if (sessionId.empty())
    {
        response.status = 400;
        return;
    }

    const auto session = FindSession(sessionId);

    if (!session)
    {
        response.status = 404;
        return;
    }

    response.status = 200;
    response.set_header("Cache-Control", "no-cache");
    response.set_header("Connection", "keep-alive");
    response.set_chunked_content_provider(
        "text/event-stream",
        [session](std::size_t offset, httplib::DataSink& sink) -> bool
        {
            std::string payload;

            if (offset == 0)
            {
                // 建议:连接建立时发送 retry 字段,便于断线后按间隔重连;
                // 注释行对客户端事件流不可见,仅作连接确认。
                payload = "retry: 5000\n\n: connected\n\n";
            }
            else
            {
                {
                    std::unique_lock<std::mutex> lock(session->streamMutex);

                    session->streamCv.wait_for(
                        lock,
                        kSseHeartbeatInterval,
                        [&session]
                        {
                            return !session->pendingEvents.empty() || session->closed;
                        });

                    if (session->closed)
                    {
                        return false;
                    }

                    if (!session->pendingEvents.empty())
                    {
                        payload = std::move(session->pendingEvents.front());
                        session->pendingEvents.pop_front();
                    }
                }

                if (payload.empty())
                {
                    payload = ": keep-alive\n\n";
                }
            }

            if (!sink.is_writable())
            {
                return false;
            }

            return sink.write(payload.data(), payload.size());
        },
        [session](bool /*success*/)
        {
            (void)session;
        });
}

void McpServer::HandleDelete(
    const httplib::Request& request,
    httplib::Response& response)
{
    if (!IsOriginAllowed(request.get_header_value("Origin")))
    {
        response.status = 403;
        return;
    }

    const std::string sessionId = request.get_header_value("Mcp-Session-Id");

    if (sessionId.empty())
    {
        response.status = 400;
        return;
    }

    SessionPtr session;

    {
        std::lock_guard<std::mutex> lock(sessionsMutex_);

        const auto it = sessions_.find(sessionId);

        if (it == sessions_.end())
        {
            response.status = 404;
            return;
        }

        session = it->second;
        sessions_.erase(it);
    }

    {
        std::lock_guard<std::mutex> lock(session->streamMutex);
        session->closed = true;
    }

    session->streamCv.notify_all();

    response.status = 200;
}

json McpServer::ProcessMessage(
    SessionPtr& session,
    const json& message,
    bool& sessionCreated)
{
    if (!message.is_object())
    {
        return BuildError(json(), -32600, "Invalid Request");
    }

    const bool hasMethod =
        message.contains("method") && message["method"].is_string();

    if (!hasMethod)
    {
        // JSON-RPC 响应消息(客户端对我们所发请求的应答)。
        // 本服务端不会主动向客户端发起请求,忽略即可。
        return json();
    }

    if (message.contains("id") && message["id"].is_null())
    {
        return BuildError(json(), -32600, "Invalid Request: id must not be null");
    }

    const std::string method = message["method"].get<std::string>();

    if (!message.contains("id"))
    {
        // 通知:不产生应答。
        if (method == "notifications/initialized" && session)
        {
            session->initialized.store(true);
        }

        return json();
    }

    const json& id = message["id"];

    if (!id.is_string() && !id.is_number())
    {
        return BuildError(json(), -32600, "Invalid Request: id must be a string or number");
    }

    const json params =
        message.contains("params") && message["params"].is_object()
            ? message["params"]
            : json::object();

    try
    {
        return ProcessRequest(session, id, method, params, sessionCreated);
    }
    catch (const McpError& e)
    {
        // 协议层错误(如参数校验失败)映射为 JSON-RPC error 应答。
        return BuildError(id, e.code, e.what());
    }
}

json McpServer::ProcessRequest(
    SessionPtr& session,
    const json& id,
    const std::string& method,
    const json& params,
    bool& sessionCreated)
{
    if (method == "initialize")
    {
        return HandleInitialize(session, id, params, sessionCreated);
    }

    if (!session)
    {
        return BuildError(id, -32600, "Invalid Request: no session");
    }

    if (method == "ping")
    {
        return BuildResult(id, json::object());
    }

    if (method == "tools/list")
    {
        return BuildResult(id, BuildToolsListResult());
    }

    if (method == "tools/call")
    {
        return BuildResult(id, ProcessToolCall(params));
    }

    return BuildError(id, -32601, "Method not found: " + method);
}

json McpServer::HandleInitialize(
    SessionPtr& session,
    const json& id,
    const json& params,
    bool& sessionCreated)
{
    std::string negotiated = kLatestProtocolVersion;

    const auto versionIt = params.find("protocolVersion");

    if (versionIt != params.end() &&
        versionIt->is_string() &&
        IsSupportedVersion(versionIt->get<std::string>()))
    {
        negotiated = versionIt->get<std::string>();
    }

    session = std::make_shared<Session>(GenerateSessionId());
    session->protocolVersion = negotiated;

    {
        std::lock_guard<std::mutex> lock(sessionsMutex_);
        sessions_[session->id] = session;
    }

    sessionCreated = true;

    json result{
        { "protocolVersion", negotiated },
        { "capabilities", BuildCapabilities() },
        { "serverInfo", BuildServerInfo() }
    };

    if (!instructions_.empty())
    {
        result["instructions"] = instructions_;
    }

    return BuildResult(id, result);
}

json McpServer::ProcessToolCall(const json& params)
{
    const auto nameIt = params.find("name");

    if (nameIt == params.end() || !nameIt->is_string())
    {
        throw McpError(-32602, "Invalid params: 'name' is required");
    }

    const std::string name = nameIt->get<std::string>();

    const Tool* tool = nullptr;

    for (const auto& candidate : tools_)
    {
        if (candidate.name == name)
        {
            tool = &candidate;
            break;
        }
    }

    if (!tool)
    {
        throw McpError(-32602, "Unknown tool: " + name);
    }

    json arguments = json::object();

    const auto argumentsIt = params.find("arguments");

    if (argumentsIt != params.end())
    {
        if (!argumentsIt->is_object())
        {
            throw McpError(-32602, "Invalid params: 'arguments' must be an object");
        }

        arguments = *argumentsIt;
    }

    ToolResult toolResult;

    try
    {
        toolResult = tool->handler(arguments);
    }
    catch (const McpError&)
    {
        // 参数错误属于协议层错误,向上传递为 JSON-RPC error 应答。
        throw;
    }
    catch (const std::exception& e)
    {
        // 工具执行失败:以 isError 结果返回,便于模型理解并自我修正。
        toolResult.content = json::array({ MakeTextContent(e.what()) });
        toolResult.isError = true;
    }
    catch (...)
    {
        toolResult.content = json::array({ MakeTextContent("unknown tool error") });
        toolResult.isError = true;
    }

    json result{ { "content", toolResult.content } };

    if (toolResult.isError)
    {
        result["isError"] = true;
    }

    if (toolResult.hasStructuredContent)
    {
        result["structuredContent"] = toolResult.structuredContent;
    }

    return result;
}

json McpServer::BuildToolsListResult() const
{
    json tools = json::array();

    for (const auto& tool : tools_)
    {
        json item{
            { "name", tool.name },
            { "description", tool.description },
            { "inputSchema", tool.inputSchema }
        };

        if (!tool.title.empty())
        {
            item["title"] = tool.title;
        }

        tools.push_back(std::move(item));
    }

    return json{ { "tools", tools } };
}

json McpServer::BuildCapabilities() const
{
    // 当前仅提供 tools 能力;工具列表为静态,不支持 listChanged 通知。
    return json{
        { "tools", json{ { "listChanged", false } } }
    };
}

json McpServer::BuildServerInfo() const
{
    json info{
        { "name", serverName_ },
        { "version", serverVersion_ }
    };

    if (!serverTitle_.empty())
    {
        info["title"] = serverTitle_;
    }

    return info;
}

json McpServer::BuildResult(const json& id, const json& result) const
{
    return json{
        { "jsonrpc", "2.0" },
        { "id", id },
        { "result", result }
    };
}

json McpServer::BuildError(const json& id, int code, const std::string& message) const
{
    return json{
        { "jsonrpc", "2.0" },
        { "id", id },
        { "error", json{
            { "code", code },
            { "message", message }
        } }
    };
}

McpServer::SessionPtr McpServer::FindSession(const std::string& sessionId) const
{
    std::lock_guard<std::mutex> lock(sessionsMutex_);

    const auto it = sessions_.find(sessionId);

    return it == sessions_.end() ? nullptr : it->second;
}

std::string McpServer::GenerateSessionId()
{
    // 32 个十六进制字符,均为可见 ASCII(0x21-0x7E),满足会话 ID 规范。
    static std::mutex randomMutex;
    static std::mt19937_64 generator(std::random_device{}());

    std::uint64_t values[2] = {};

    {
        std::lock_guard<std::mutex> lock(randomMutex);
        values[0] = generator();
        values[1] = generator();
    }

    char buffer[33]{};

    std::snprintf(
        buffer,
        sizeof(buffer),
        "%016llx%016llx",
        static_cast<unsigned long long>(values[0]),
        static_cast<unsigned long long>(values[1]));

    return buffer;
}

bool McpServer::IsOriginAllowed(const std::string& origin) const
{
    // 非浏览器客户端(如 curl、MCP 客户端库)通常不发送 Origin,予以放行;
    // 浏览器发送的 Origin 仅允许回环地址,防止 DNS rebinding 攻击。
    if (origin.empty())
    {
        return true;
    }

    std::size_t hostBegin = std::string::npos;

    if (origin.rfind("http://", 0) == 0)
    {
        hostBegin = 7;
    }
    else if (origin.rfind("https://", 0) == 0)
    {
        hostBegin = 8;
    }

    if (hostBegin == std::string::npos)
    {
        return false;
    }

    const auto hostEnd = origin.find_first_of(":/", hostBegin);
    const std::string host = origin.substr(
        hostBegin,
        hostEnd == std::string::npos ? std::string::npos : hostEnd - hostBegin);

    return host == "localhost" || host == "127.0.0.1" || host == "[::1]";
}

bool McpServer::IsSupportedVersion(const std::string& version) const
{
    for (const auto* supported : kSupportedVersions)
    {
        if (version == supported)
        {
            return true;
        }
    }

    return false;
}

bool McpServer::BatchingAllowed(const std::string& version)
{
    // JSON-RPC 批量消息仅 2025-06-18 之前的版本支持。
    return version == "2024-11-05" || version == "2025-03-26";
}

void McpServer::SetJsonBody(httplib::Response& response, int status, const json& value)
{
    response.status = status;
    response.set_content(value.dump(), "application/json; charset=utf-8");
}

json McpServer::MakeTextContent(const std::string& text)
{
    return json{
        { "type", "text" },
        { "text", text }
    };
}

void McpServer::PushEvent(const SessionPtr& session, const json& message)
{
    if (!session || message.is_null())
    {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(session->streamMutex);
        session->pendingEvents.push_back("event: message\ndata: " + message.dump() + "\n\n");
    }

    session->streamCv.notify_all();
}
