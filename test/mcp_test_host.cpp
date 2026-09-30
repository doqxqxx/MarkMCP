// mcp_test_host.cpp : 测试宿主程序。
// 模拟真实宿主:加载 plug1.dll 并调用导出函数 GetMarks(阻塞直至 HTTP 服务停止),
// 验证 MCP 服务全流程与"提交后服务自动停止、GetMarks 返回"的新语义。
//
// 用法: mcp_test_host.exe <plug1.dll 路径> [回调返回值:1=成功(默认) 0=失败]

#include <windows.h>

#include <cstdio>
#include <cstring>

typedef bool (*pfn_AddMarks)(size_t StartAddress, size_t EndAddress);
typedef void (*pfn_GetMarks)(pfn_AddMarks);

static bool g_callbackSucceeds = true;
static volatile long g_callbackCount = 0;

static bool CALLBACK AddMarks(size_t startAddress, size_t endAddress)
{
    InterlockedIncrement(&g_callbackCount);
    std::printf("[host] AddMarks(start=0x%zx, end=0x%zx) -> %s\n",
        startAddress, endAddress, g_callbackSucceeds ? "true" : "false");
    return g_callbackSucceeds;
}

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::printf("usage: mcp_test_host.exe <plug1.dll path> [callback returns 0|1]\n");
        return 2;
    }

    if (argc >= 3 && std::strcmp(argv[2], "0") == 0)
    {
        g_callbackSucceeds = false;
    }

    HMODULE dll = LoadLibraryA(argv[1]);
    if (!dll)
    {
        std::printf("[host] LoadLibrary failed: %lu\n", GetLastError());
        return 1;
    }

    const auto getMarks = reinterpret_cast<pfn_GetMarks>(GetProcAddress(dll, "GetMarks"));
    if (!getMarks)
    {
        std::printf("[host] GetProcAddress(GetMarks) failed: %lu\n", GetLastError());
        FreeLibrary(dll);
        return 1;
    }

    std::printf("[host] calling GetMarks (blocks until HTTP service stops)...\n");
    getMarks(&AddMarks);
    std::printf("[host] GetMarks returned after %ld callback(s), service stopped.\n", g_callbackCount);

    FreeLibrary(dll);
    return 0;
}
