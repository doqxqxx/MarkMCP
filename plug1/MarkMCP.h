#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

#include "httplib.h"
#include "plug_v1.h"
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

    MarkMCP() = default;
    ~MarkMCP()
    {
        StopServer();
    }

    bool StartServer(int port);
    void StopServer();
    bool CheckPort(int port) const;
    bool IsRunning() const { return isRunning.load(); }
    bool IsDone() const { return isDone.load(); }
    void SetAddMarksFunction(pfn_AddMarks addMarksFunction) { addMarksFunction = addMarksFunction; }

private:
	pfn_AddMarks addMarksFunction{ nullptr };
    std::uint64_t AddMark(std::uintptr_t startAddress, std::uintptr_t endAddress);
    bool UpdateMark(std::uint64_t id, std::uintptr_t startAddress, std::uintptr_t endAddress);
    bool DeleteMark(std::uint64_t id);
    bool GetMark(std::uint64_t id, Mark& mark) const;
    std::vector<Mark> GetAllMarks() const;
    bool IsValidRange(std::uintptr_t startAddress, std::uintptr_t endAddress) const;
	bool AddMarkToExternal(std::uintptr_t startAddress, std::uintptr_t endAddress) const;
	bool AddAllMarksToExternal() const;

    httplib::Server server{};
    mutable std::mutex marksMutex;
    std::vector<Mark> marks;
    std::atomic<std::uint64_t> nextId{ 1 };
    std::atomic<bool> isDone{ false };
    std::atomic<bool> isRunning{ false };
};