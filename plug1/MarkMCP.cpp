#include "MarkMCP.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <stdexcept>
#include <utility>

#include "McpServer.h"
#include "json.hpp"

#pragma comment(lib, "Ws2_32.lib")

using json = nlohmann::json;

namespace
{
    void SetJson(
        httplib::Response& response,
        const json& value,
        int status = 200)
    {
        response.status = status;
        response.set_content(
            value.dump(),
            "application/json; charset=utf-8");
    }

    void SetError(
        httplib::Response& response,
        int status,
        const char* message)
    {
        SetJson(
            response,
            json{
                {"success", false},
                {"error", message}
            },
            status);
    }

    bool TryParseId(
        const httplib::Request& request,
        std::uint64_t& id)
    {
        try
        {
            id = std::stoull(request.matches[1].str());
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    bool TryGetAddress(
        const json& body,
        const char* name,
        std::uintptr_t& address)
    {
        if (!body.contains(name))
        {
            return false;
        }

        try
        {
            if (body[name].is_string())
            {
                address = static_cast<std::uintptr_t>(
                    std::stoull(body[name].get<std::string>(), nullptr, 0));
            }
            else
            {
                address = static_cast<std::uintptr_t>(
                    body[name].get<std::uint64_t>());
            }

            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    // 以下为 MCP 工具层辅助函数

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

    // 地址统一以 0x 前缀十六进制字符串返回,避免 64 位整数在 JS 等客户端丢失精度。
    std::string ToHexAddress(std::uintptr_t address)
    {
        char buffer[32]{};
        std::snprintf(
            buffer,
            sizeof(buffer),
            "0x%llx",
            static_cast<unsigned long long>(address));
        return buffer;
    }

    json MarkToJson(const MarkMCP::Mark& mark)
    {
        return json{
            { "id", mark.id },
            { "startAddress", ToHexAddress(mark.startAddress) },
            { "endAddress", ToHexAddress(mark.endAddress) }
        };
    }

    McpServer::ToolResult MakeStructuredResult(const std::string& text, json structured)
    {
        McpServer::ToolResult result;
        result.content = json::array({
            json{
                { "type", "text" },
                { "text", text }
            }
            });
        result.structuredContent = std::move(structured);
        result.hasStructuredContent = true;
        return result;
    }

    json StringProperty(const char* description)
    {
        return json{
            { "type", "string" },
            { "description", description }
        };
    }

    json IdProperty(const char* description)
    {
        return json{
            { "type", "integer" },
            { "description", description },
            { "minimum", 0 }
        };
    }

    json ObjectSchema(json properties, json required)
    {
        return json{
            { "type", "object" },
            { "properties", std::move(properties) },
            { "required", std::move(required) },
            { "additionalProperties", false }
        };
    }

    // 解析工具参数中的地址:支持 JSON 整数,或十进制 / 0x 前缀十六进制字符串。
    std::uintptr_t ParseAddressValue(const json& value, const char* name)
    {
        const std::string error = std::string("Invalid params: '") + name +
            "' must be an integer, or a decimal / 0x-hex string";

        if (value.is_number_unsigned())
        {
            return static_cast<std::uintptr_t>(value.get<std::uint64_t>());
        }

        if (value.is_number_integer())
        {
            const auto signedValue = value.get<std::int64_t>();

            if (signedValue < 0)
            {
                throw McpServer::McpError(-32602, error);
            }

            return static_cast<std::uintptr_t>(signedValue);
        }

        if (!value.is_string())
        {
            throw McpServer::McpError(-32602, error);
        }

        std::string text = TrimCopy(value.get<std::string>());

        int base = 10;

        if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))
        {
            base = 16;
            text.erase(0, 2);
        }

        if (text.empty() || text[0] == '-' || text[0] == '+')
        {
            throw McpServer::McpError(-32602, error);
        }

        try
        {
            std::size_t parsed = 0;
            const auto address = std::stoull(text, &parsed, base);

            if (parsed != text.size())
            {
                throw std::invalid_argument("trailing characters");
            }

            return static_cast<std::uintptr_t>(address);
        }
        catch (...)
        {
            throw McpServer::McpError(-32602, error);
        }
    }

    std::uintptr_t RequireAddress(const json& arguments, const char* name)
    {
        const auto it = arguments.find(name);

        if (it == arguments.end())
        {
            throw McpServer::McpError(
                -32602,
                std::string("Invalid params: '") + name + "' is required");
        }

        return ParseAddressValue(*it, name);
    }

    std::uint64_t RequireId(const json& arguments)
    {
        const auto it = arguments.find("id");

        if (it == arguments.end())
        {
            throw McpServer::McpError(-32602, "Invalid params: 'id' is required");
        }

        if (it->is_number_unsigned())
        {
            return it->get<std::uint64_t>();
        }

        if (it->is_number_integer())
        {
            const auto value = it->get<std::int64_t>();

            if (value >= 0)
            {
                return static_cast<std::uint64_t>(value);
            }
        }

        throw McpServer::McpError(
            -32602,
            "Invalid params: 'id' must be a non-negative integer");
    }
}

bool MarkMCP::StartServer(int port)
{
    if (!CheckPort(port))
    {
        return false;
    }

    isDone.store(false);
    isRunning.store(true);

    // 查询全部
    server.Get("/marks",
        [this](const httplib::Request&, httplib::Response& response)
        {
            const auto allMarks = GetAllMarks();

            json result = json::array();

            for (const auto& mark : allMarks)
            {
                result.push_back({
                    {"id", mark.id},
                    {"startAddress", mark.startAddress},
                    {"endAddress", mark.endAddress}
                    });
            }

            SetJson(response, result);
        });

    // 查询单个
    server.Get(R"(/marks/(\d+))",
        [this](const httplib::Request& request,
            httplib::Response& response)
        {
            std::uint64_t id{};

            if (!TryParseId(request, id))
            {
                SetError(response, 400, "invalid id");
                return;
            }

            Mark mark{};

            if (!GetMark(id, mark))
            {
                SetError(response, 404, "mark not found");
                return;
            }

            SetJson(response, {
                {"id", mark.id},
                {"startAddress", mark.startAddress},
                {"endAddress", mark.endAddress}
                });
        });

    // 新增
    server.Post("/marks",
        [this](const httplib::Request& request,
            httplib::Response& response)
        {
            try
            {
                const auto body = json::parse(request.body);

                std::uintptr_t startAddress{};
                std::uintptr_t endAddress{};

                if (!TryGetAddress(body, "startAddress", startAddress) ||
                    !TryGetAddress(body, "endAddress", endAddress))
                {
                    SetError(
                        response,
                        400,
                        "startAddress and endAddress are required");
                    return;
                }

                if (!IsValidRange(startAddress, endAddress))
                {
                    SetError(response, 400, "invalid address range");
                    return;
                }

                const auto id = AddMark(startAddress, endAddress);

                SetJson(response, {
                    {"success", true},
                    {"id", id}
                    }, 201);
            }
            catch (const json::exception&)
            {
                SetError(response, 400, "invalid json");
            }
        });

    // 修改
    server.Put(R"(/marks/(\d+))",
        [this](const httplib::Request& request,
            httplib::Response& response)
        {
            std::uint64_t id{};

            if (!TryParseId(request, id))
            {
                SetError(response, 400, "invalid id");
                return;
            }

            try
            {
                const auto body = json::parse(request.body);

                std::uintptr_t startAddress{};
                std::uintptr_t endAddress{};

                if (!TryGetAddress(body, "startAddress", startAddress) ||
                    !TryGetAddress(body, "endAddress", endAddress))
                {
                    SetError(
                        response,
                        400,
                        "startAddress and endAddress are required");
                    return;
                }

                if (!IsValidRange(startAddress, endAddress))
                {
                    SetError(response, 400, "invalid address range");
                    return;
                }

                if (!UpdateMark(id, startAddress, endAddress))
                {
                    SetError(response, 404, "mark not found");
                    return;
                }

                SetJson(response, {
                    {"success", true},
                    {"id", id}
                    });
            }
            catch (const json::exception&)
            {
                SetError(response, 400, "invalid json");
            }
        });

    // 删除
    server.Delete(R"(/marks/(\d+))",
        [this](const httplib::Request& request,
            httplib::Response& response)
        {
            std::uint64_t id{};

            if (!TryParseId(request, id))
            {
                SetError(response, 400, "invalid id");
                return;
            }

            if (!DeleteMark(id))
            {
                SetError(response, 404, "mark not found");
                return;
            }

            SetJson(response, {
                {"success", true},
                {"id", id}
                });
        });

    // 状态
    server.Get("/status",
        [this](const httplib::Request&, httplib::Response& response)
        {
            SetJson(response, {
                {"running", IsRunning()},
                {"done", IsDone()},
                {"markCount", GetAllMarks().size()}
                });
        });

    // 停止服务
    server.Get("/stop",
        [this](const httplib::Request&, httplib::Response& response)
        {
            isDone.store(true);
            response.set_content(
                R"({"success":true})",
                "application/json; charset=utf-8");
            if (mcpServer)
            {
                // 先结束 MCP SSE 长连接并清空会话,再停止 HTTP 服务,
                // 避免长连接占用工作线程导致服务无法退出。
                mcpServer->Shutdown();
            }
            server.stop();
            AddAllMarksToExternal();
        });

    // 标准 MCP 协议端点(Streamable HTTP /mcp),与 REST API 共用同一 HTTP 服务。
    mcpServer = std::make_unique<McpServer>();
    mcpServer->SetServerInfo("MarkMCP", "MarkMCP 地址标记服务", "1.0.0");
    mcpServer->SetInstructions(
        "管理宿主进程的内存地址区间标记(mark)。"
        "典型流程:用 add_mark 添加地址区间(地址支持 \"0x1000\" 十六进制或十进制字符串),"
        "用 list_marks / get_mark 查询,update_mark / delete_mark 修改或删除;"
        "确认后调用 submit_marks 将全部标记提交给宿主程序——提交完成后服务会自动停止,这是最终步骤。");
    RegisterMcpTools(*mcpServer);
    mcpServer->RegisterRoutes(server);

    const bool result = server.listen("127.0.0.1", port);

    isRunning.store(false);
    isDone.store(true);

    return result;
}

MarkMCP::MarkMCP() = default;

MarkMCP::~MarkMCP()
{
    StopServer();
}

void MarkMCP::StopServer()
{
    if (isRunning.load())
    {
        isDone.store(true);

        if (mcpServer)
        {
            // 先结束 MCP SSE 长连接,再停止 HTTP 服务。
            mcpServer->Shutdown();
        }

        server.stop();
        isRunning.store(false);
    }
}

std::uint64_t MarkMCP::AddMark(
    std::uintptr_t startAddress,
    std::uintptr_t endAddress)
{
    std::lock_guard<std::mutex> lock(marksMutex);

    const auto id = nextId.fetch_add(1);

    marks.push_back({
        id,
        startAddress,
        endAddress
        });

    return id;
}

bool MarkMCP::UpdateMark(
    std::uint64_t id,
    std::uintptr_t startAddress,
    std::uintptr_t endAddress)
{
    std::lock_guard<std::mutex> lock(marksMutex);

    const auto it = std::find_if(
        marks.begin(),
        marks.end(),
        [id](const Mark& mark)
        {
            return mark.id == id;
        });

    if (it == marks.end())
    {
        return false;
    }

    it->startAddress = startAddress;
    it->endAddress = endAddress;

    return true;
}

bool MarkMCP::DeleteMark(std::uint64_t id)
{
    std::lock_guard<std::mutex> lock(marksMutex);

    const auto oldSize = marks.size();

    marks.erase(
        std::remove_if(
            marks.begin(),
            marks.end(),
            [id](const Mark& mark)
            {
                return mark.id == id;
            }),
        marks.end());

    return marks.size() != oldSize;
}

bool MarkMCP::GetMark(std::uint64_t id, Mark& mark) const
{
    std::lock_guard<std::mutex> lock(marksMutex);

    const auto it = std::find_if(
        marks.begin(),
        marks.end(),
        [id](const Mark& mark)
        {
            return mark.id == id;
        });

    if (it == marks.end())
    {
        return false;
    }

    mark = *it;
    return true;
}

std::vector<MarkMCP::Mark> MarkMCP::GetAllMarks() const
{
    std::lock_guard<std::mutex> lock(marksMutex);
    return marks;
}

bool MarkMCP::IsValidRange(
    std::uintptr_t startAddress,
    std::uintptr_t endAddress) const
{
    return startAddress < endAddress;
}

std::vector<MarkMCP::SubmitResult> MarkMCP::SubmitAllMarks() const
{
    const auto allMarks = GetAllMarks();

    std::vector<SubmitResult> results;
    results.reserve(allMarks.size());

    for (const auto& mark : allMarks)
    {
        bool succeeded = false;

        if (addMarksFunction)
        {
            try
            {
                succeeded = addMarksFunction(mark.startAddress, mark.endAddress);
            }
            catch (...)
            {
                succeeded = false;
            }
        }

        results.push_back({ mark, succeeded });
    }

    return results;
}

bool MarkMCP::AddAllMarksToExternal() const
{
    const auto allMarks = GetAllMarks();
    const auto results = SubmitAllMarks();

    std::vector<Mark> failedMarks;

    for (const auto& entry : results)
    {
        if (!entry.success)
        {
            failedMarks.push_back(entry.mark);
        }
    }

    if (failedMarks.empty())
    {
        return true;
    }

    std::ofstream logFile("addMarks_errors.log", std::ios::app);
    if (logFile)
    {
        const auto now = std::time(nullptr);
        std::tm localTime{};
        const bool hasLocalTime = localtime_s(&localTime, &now) == 0;
        logFile << "\n=== addMarksFunction failure ===\n";
        if (hasLocalTime)
        {
            logFile << "Time: " << std::put_time(&localTime, "%Y-%m-%d %H:%M:%S") << '\n';
        }
        logFile << "All marks (" << allMarks.size() << "): \n";
        for (const auto& mark : allMarks)
        {
            logFile << "id=" << mark.id
                << " start=0x" << std::hex << mark.startAddress
                << " end=0x" << mark.endAddress << std::dec << '\n';
        }
        logFile << "Failed marks (" << failedMarks.size() << "): \n";
        for (const auto& mark : failedMarks)
        {
            logFile << "id=" << mark.id
                << " start=0x" << std::hex << mark.startAddress
                << " end=0x" << mark.endAddress << std::dec << '\n';
        }
    }
    logFile.flush();

	MessageBoxA(nullptr,
		"Failed to add some marks to the external function. See addMarks_errors.log for details.",
		"MarkMCP Error",
		MB_ICONERROR | MB_OK);
    

    return false;
}

bool MarkMCP::CheckPort(int port) const
{
    if (port <= 0 || port > 65535)
    {
        return false;
    }

    WSADATA wsaData{};

    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
    {
        return false;
    }

    SOCKET socketHandle = socket(
        AF_INET,
        SOCK_STREAM,
        IPPROTO_TCP);

    if (socketHandle == INVALID_SOCKET)
    {
        WSACleanup();
        return false;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(static_cast<u_short>(port));

    const int result = bind(
        socketHandle,
        reinterpret_cast<sockaddr*>(&address),
        sizeof(address));

    const bool available = result != SOCKET_ERROR;

    closesocket(socketHandle);
    WSACleanup();

    return available;
}

void MarkMCP::RegisterMcpTools(McpServer& mcpServer)
{
    using Tool = McpServer::Tool;

    mcpServer.AddTool(Tool{
        "list_marks",
        "列出全部标记",
        "获取当前保存的全部 mark(内存地址区间)列表,按创建顺序返回。"
        "地址以 0x 前缀十六进制字符串表示,避免 64 位整数在 JSON 中丢失精度。",
        ObjectSchema(json::object(), json::array()),
        [this](const json&)
        {
            const auto allMarks = GetAllMarks();

            json items = json::array();

            for (const auto& mark : allMarks)
            {
                items.push_back(MarkToJson(mark));
            }

            json structured{
                { "count", allMarks.size() },
                { "marks", items }
            };

            return MakeStructuredResult(items.dump(2), std::move(structured));
        }
        });

    mcpServer.AddTool(Tool{
        "get_mark",
        "查询单个标记",
        "按 id 查询单个 mark,返回其地址区间。",
        ObjectSchema(
            json{ { "id", IdProperty("mark 的 id") } },
            json::array({ "id" })),
        [this](const json& arguments)
        {
            const auto id = RequireId(arguments);

            Mark mark{};

            if (!GetMark(id, mark))
            {
                throw std::runtime_error("mark not found: id=" + std::to_string(id));
            }

            const json data = MarkToJson(mark);
            return MakeStructuredResult(data.dump(2), data);
        }
        });

    mcpServer.AddTool(Tool{
        "add_mark",
        "新增标记",
        "新增一个 mark(内存地址区间)。地址支持十进制或 0x 前缀十六进制字符串;"
        "要求 startAddress 小于 endAddress。",
        ObjectSchema(
            json{
                { "startAddress", StringProperty("起始地址,如 \"0x1000\" 或 \"4096\"") },
                { "endAddress", StringProperty("结束地址,必须大于起始地址") }
            },
            json::array({ "startAddress", "endAddress" })),
        [this](const json& arguments)
        {
            const auto startAddress = RequireAddress(arguments, "startAddress");
            const auto endAddress = RequireAddress(arguments, "endAddress");

            if (startAddress >= endAddress)
            {
                throw std::runtime_error(
                    "invalid address range: startAddress must be less than endAddress");
            }

            const auto id = AddMark(startAddress, endAddress);

            const json data = MarkToJson({ id, startAddress, endAddress });
            return MakeStructuredResult("mark added: " + data.dump(), data);
        }
        });

    mcpServer.AddTool(Tool{
        "update_mark",
        "修改标记",
        "按 id 修改已有 mark 的地址区间。地址支持十进制或 0x 前缀十六进制字符串。",
        ObjectSchema(
            json{
                { "id", IdProperty("要修改的 mark 的 id") },
                { "startAddress", StringProperty("新的起始地址") },
                { "endAddress", StringProperty("新的结束地址,必须大于起始地址") }
            },
            json::array({ "id", "startAddress", "endAddress" })),
        [this](const json& arguments)
        {
            const auto id = RequireId(arguments);
            const auto startAddress = RequireAddress(arguments, "startAddress");
            const auto endAddress = RequireAddress(arguments, "endAddress");

            if (startAddress >= endAddress)
            {
                throw std::runtime_error(
                    "invalid address range: startAddress must be less than endAddress");
            }

            if (!UpdateMark(id, startAddress, endAddress))
            {
                throw std::runtime_error("mark not found: id=" + std::to_string(id));
            }

            const json data = MarkToJson({ id, startAddress, endAddress });
            return MakeStructuredResult("mark updated: " + data.dump(), data);
        }
        });

    mcpServer.AddTool(Tool{
        "delete_mark",
        "删除标记",
        "按 id 删除一个 mark。",
        ObjectSchema(
            json{ { "id", IdProperty("要删除的 mark 的 id") } },
            json::array({ "id" })),
        [this](const json& arguments)
        {
            const auto id = RequireId(arguments);

            if (!DeleteMark(id))
            {
                throw std::runtime_error("mark not found: id=" + std::to_string(id));
            }

            json structured{
                { "deleted", true },
                { "id", id }
            };

            return MakeStructuredResult(
                "mark deleted: id=" + std::to_string(id),
                std::move(structured));
        }
        });

    mcpServer.AddTool(Tool{
        "get_status",
        "查询服务状态",
        "查询 mark 服务的运行状态与当前 mark 数量。",
        ObjectSchema(json::object(), json::array()),
        [this](const json&)
        {
            json structured{
                { "running", IsRunning() },
                { "done", IsDone() },
                { "markCount", GetAllMarks().size() }
            };

            return MakeStructuredResult(structured.dump(2), std::move(structured));
        }
        });

    mcpServer.AddTool(Tool{
        "submit_marks",
        "提交全部标记并结束服务",
        "将当前全部 mark 逐项提交给宿主程序(调用宿主注册的回调),这是最终步骤:"
        "提交完成后 HTTP/MCP 服务会自动停止,不可再修改或再次提交,"
        "因此提交前请先用 list_marks 等工具核对全部条目。若任一提交失败,结果为 isError。",
        ObjectSchema(json::object(), json::array()),
        [this](const json&)
        {
            const auto results = SubmitAllMarks();

            json items = json::array();
            std::size_t failedCount = 0;

            for (const auto& entry : results)
            {
                items.push_back(json{
                    { "id", entry.mark.id },
                    { "startAddress", ToHexAddress(entry.mark.startAddress) },
                    { "endAddress", ToHexAddress(entry.mark.endAddress) },
                    { "success", entry.success }
                    });

                if (!entry.success)
                {
                    ++failedCount;
                }
            }

            // 提交即结束:停止服务,让宿主程序继续运行。
            // 先提交后停止,响应会在停止前写回(与 REST /stop 相同模式,已验证)。
            StopServer();

            json structured{
                { "total", results.size() },
                { "failed", failedCount },
                { "serviceStopped", true },
                { "results", items }
            };

            std::string text =
                "submitted " + std::to_string(results.size()) + " mark(s) to host, service stopped";

            if (failedCount > 0)
            {
                text += ", " + std::to_string(failedCount) + " failed";
            }

            auto toolResult = MakeStructuredResult(std::move(text), std::move(structured));
            toolResult.isError = failedCount > 0;
            return toolResult;
        }
        });
}