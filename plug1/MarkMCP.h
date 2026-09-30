#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include "httplib.h"
#include "plug_v1.h"

class McpServer;

class MarkMCP
{
public:
    struct Mark
    {
        std::uint64_t id{};
        std::uintptr_t startAddress{};
        std::uintptr_t endAddress{};

        bool IsValid() const
        {
            return startAddress < endAddress;
        }
    };

    // 单个 mark 提交给宿主回调的结果。
    struct SubmitResult
    {
        Mark mark;
        bool success{};
    };

    // pimpl 约定:持有 unique_ptr<McpServer> 的特殊成员必须在
    // McpServer 完整类型可见的 MarkMCP.cpp 中定义,不能在头文件内 default。
    MarkMCP();
    ~MarkMCP();

    bool StartServer(int port);
    void StopServer();
    bool CheckPort(int port) const;
    bool IsRunning() const { return isRunning.load(); }
    bool IsDone() const { return isDone.load(); }
    void SetAddMarksFunction(pfn_AddMarks addMarksFunction) { this->addMarksFunction = addMarksFunction; }

    // 以下接口供 MCP 工具层调用。
    std::uint64_t AddMark(std::uintptr_t startAddress, std::uintptr_t endAddress);
    bool UpdateMark(std::uint64_t id, std::uintptr_t startAddress, std::uintptr_t endAddress);
    bool DeleteMark(std::uint64_t id);
    bool GetMark(std::uint64_t id, Mark& mark) const;
    std::vector<Mark> GetAllMarks() const;

    // 逐项调用宿主回调提交所有 mark,返回每项结果;不弹窗、不写日志。
    std::vector<SubmitResult> SubmitAllMarks() const;

private:
    void RegisterMcpTools(McpServer& mcpServer);

    // REST /stop 专用:提交全部 mark,失败时写日志并弹窗。
    bool AddAllMarksToExternal() const;
    bool IsValidRange(std::uintptr_t startAddress, std::uintptr_t endAddress) const;

    pfn_AddMarks addMarksFunction{ nullptr };
    httplib::Server server{};
    std::unique_ptr<McpServer> mcpServer;
    mutable std::mutex marksMutex;
    std::vector<Mark> marks;
    std::atomic<std::uint64_t> nextId{ 1 };
    std::atomic<bool> isDone{ false };
    std::atomic<bool> isRunning{ false };
};
