// dllmain.cpp : 定义 DLL 应用程序的入口点。


#include "MarkMCP.h"

#include <cstdio>
#include <windows.h>

extern "C" {

	__declspec(dllexport)
		void GetMarks(pfn_AddMarks pAddMarks) {
			MarkMCP markMCP;
			markMCP.SetAddMarksFunction(pAddMarks);
			markMCP.StartServer(8080);
		}

}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
	switch (ul_reason_for_call)
	{
	case DLL_PROCESS_ATTACH:
		break;
	case DLL_THREAD_ATTACH:
		break;
	case DLL_THREAD_DETACH:
		break;
	case DLL_PROCESS_DETACH:
		break;
	}
	return TRUE;
}

//int WinMain(_In_ HINSTANCE hInstance,
//	_In_opt_ HINSTANCE hPrevInstance,
//	_In_ LPSTR lpCmdLine,
//	_In_ int nShowCmd) {
//	AllocConsole();
//	// 重定向标准输出到控制台
//	FILE* consoleOutput = nullptr;
//	freopen_s(&consoleOutput, "CONOUT$", "w", stdout);
//
//
//
//	MarkMCP markMCP;
//	markMCP.StartServer(8080);
//	printf("MarkMCP server started on port 8080\n");
//	system("pause");
//	return 0;
//
//}