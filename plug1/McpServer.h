#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

#include "httplib.h"
#include "json.hpp"

using json = nlohmann::json;

// 标准 MCP (Model Context Protocol) 服务器实现,使用 Streamable HTTP 传输。
//
// 客户端通过 POST/GET/DELETE /mcp 与本服务交互:
//   POST   发送 JSON-RPC 消息;请求以 application/json 单对象应答,
//          通知/响应消息返回 202 Accepted。
//   GET    打开 text/event-stream 的 SSE 长连接,用于接收服务端主动推送
//          的消息(当前实现不主动推送,仅发送 keep-alive 心跳)。
//   DELETE 终止指定会话。
//
// 已实现的协议版本:2024-11-05 / 2025-03-26 / 2025-06-18 / 2025-11-25。
// 本类只负责 JSON-RPC 分发、会话管理与 SSE 通道,与具体业务解耦;
// 工具(tools)由外部通过 AddTool 注册,须在 RegisterRoutes 之前完成注册。
class McpServer
{
public:
    // 工具执行结果。content 为 MCP content 数组;
    // isError=true 表示工具执行失败(以结果形式返回,而非协议错误)。
    struct ToolResult
    {
        json content = json::array();
        json structuredContent = json::object();
        bool hasStructuredContent{ false };
        bool isError{ false };
    };

    // 协议层错误:code 为 JSON-RPC 错误码(如 -32602 Invalid params)。
    struct McpError : std::runtime_error
    {
        int code;

        McpError(int errorCode, const std::string& message)
            : std::runtime_error(message)
            , code(errorCode)
        {
        }
    };

    // handler 抛出 McpError 视为协议层错误(映射为 JSON-RPC error 应答);
    // 抛出其他异常视为工具执行失败(映射为 isError=true 的结果)。
    using ToolHandler = std::function<ToolResult(const json& arguments)>;

    struct Tool
    {
        std::string name;
        std::string title;
        std::string description;
        json inputSchema;
        ToolHandler handler;
    };

    McpServer() = default;
    ~McpServer();

    McpServer(const McpServer&) = delete;
    McpServer& operator=(const McpServer&) = delete;

    void SetServerInfo(std::string name, std::string title, std::string version);
    void SetInstructions(std::string instructions);
    void AddTool(Tool tool);

    // 在给定 HTTP 服务器上注册 MCP 端点(POST/GET/DELETE /mcp)。
    void RegisterRoutes(httplib::Server& server);

    // 结束所有 SSE 长连接并清空会话。
    // 必须在 server.stop() 之前调用,否则长连接会一直占用工作线程。
    void Shutdown();

private:
    struct Session
    {
        explicit Session(std::string sessionId)
            : id(std::move(sessionId))
        {
        }

        std::string id;
        std::string protocolVersion;
        std::atomic<bool> initialized{ false };

        // SSE 推送通道。允许多个 GET 流同时挂接,每条事件只投递给其中一个。
        std::mutex streamMutex;
        std::condition_variable streamCv;
        std::deque<std::string> pendingEvents;
        bool closed{ false };
    };

    using SessionPtr = std::shared_ptr<Session>;

    void HandlePost(const httplib::Request& request, httplib::Response& response);
    void HandleBatchPost(SessionPtr& session, const json& body, httplib::Response& response);
    void HandleGet(const httplib::Request& request, httplib::Response& response);
    void HandleDelete(const httplib::Request& request, httplib::Response& response);

    // 处理单条 JSON-RPC 消息,返回应答;通知/响应消息无应答时返回 null。
    json ProcessMessage(SessionPtr& session, const json& message, bool& sessionCreated);
    json ProcessRequest(
        SessionPtr& session,
        const json& id,
        const std::string& method,
        const json& params,
        bool& sessionCreated);
    json HandleInitialize(SessionPtr& session, const json& id, const json& params, bool& sessionCreated);
    json ProcessToolCall(const json& params);

    json BuildToolsListResult() const;
    json BuildCapabilities() const;
    json BuildServerInfo() const;
    json BuildResult(const json& id, const json& result) const;
    json BuildError(const json& id, int code, const std::string& message) const;

    SessionPtr FindSession(const std::string& sessionId) const;
    static std::string GenerateSessionId();

    bool IsOriginAllowed(const std::string& origin) const;
    bool IsSupportedVersion(const std::string& version) const;
    static bool BatchingAllowed(const std::string& version);

    static void SetJsonBody(httplib::Response& response, int status, const json& value);
    static json MakeTextContent(const std::string& text);
    // 将 JSON-RPC 消息推送到会话的 SSE 通道(当前无服务端主动推送场景,保留备用)。
    static void PushEvent(const SessionPtr& session, const json& message);

    std::string serverName_{ "mcp-server" };
    std::string serverTitle_;
    std::string serverVersion_{ "1.0.0" };
    std::string instructions_;
    std::vector<Tool> tools_;

    mutable std::mutex sessionsMutex_;
    std::map<std::string, SessionPtr> sessions_;
};
