#include "MarkMCP.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <stdexcept>

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
            server.stop();
            AddAllMarksToExternal();
        });

    const bool result = server.listen("127.0.0.1", port);

    isRunning.store(false);
    isDone.store(true);

    return result;
}

void MarkMCP::StopServer()
{
    if (isRunning.load())
    {
        isDone.store(true);
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

bool MarkMCP::AddMarkToExternal(std::uintptr_t startAddress, std::uintptr_t endAddress) const
{
    if (addMarksFunction)
    {
        return addMarksFunction(startAddress, endAddress);
    }
    return false;
}

bool MarkMCP::AddAllMarksToExternal() const
{
    const auto allMarks = GetAllMarks();
    std::vector<Mark> failedMarks;

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

        if (!succeeded)
        {
            failedMarks.push_back(mark);
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