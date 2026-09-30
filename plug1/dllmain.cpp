// dllmain.cpp : 定义 DLL 应用程序的入口点。


#include "MarkMCP.h"

#include <cstdio>
#include <windows.h>

extern "C" {

	__declspec(dllexport)
		void GetMarks(pfn_AddMarks pAddMarks) {
		MarkMCP markMCP;
		if (!markMCP.CheckPort(8080))
		{
			MessageBox(nullptr, L"MarkMCP 插件加载失败,端口 8080 被占用,请关闭占用程序后重试。", L"错误", MB_OK | MB_ICONERROR);
			return;
		}

		MessageBox(nullptr, L"MarkMCP 插件已加载,相关信息已复制到剪贴板,可以直接粘贴给Agent使用。", L"提示", MB_OK | MB_ICONINFORMATION);
		std::wstring message = LR"(# 把 MarkMCP 添加为你的 MCP 服务器并按规范使用

我的程序已经在运行,并且加载了 MarkMCP 插件,MCP 服务**现在就已经可用**,不需要你构建或启动任何东西。请你完成添加并按下面的约定使用它。

## 服务器信息
- 端点:`http://127.0.0.1:8080/mcp`(仅本机回环地址)
- 传输方式:MCP 标准 Streamable HTTP
- 建议名称:`mark-mcp`

## 你要做的添加步骤
1. 在你自己的 MCP 配置(用户级或项目级,mcpServers 段)中添加:
   `"mark-mcp": { "type": "http", "url": "http://127.0.0.1:8080/mcp" }`
2. 按你所在客户端的要求重载配置或重启会话使配置生效。
3. 如果你的客户端只支持 stdio 传输,用桥接方式添加,命令为:
   `npx mcp-remote http://127.0.0.1:8080/mcp`
4. 连接成功后先调用 `get_status` 工具,返回 `running: true` 即为正常。
5. 自检(安全,不会影响我的程序):用 `add_mark` 添加 `{"startAddress":"0x1000","endAddress":"0x2000"}`,用 `list_marks` 确认后用 `delete_mark` 删除它。

## 工具一览
| 工具 | 用途 | 参数 |
| --- | --- | --- |
| `add_mark` | 新增一个 mark(内存地址区间) | `startAddress`、`endAddress` |
| `list_marks` | 列出当前全部 mark | 无 |
| `get_mark` | 按 id 查询 | `id` |
| `update_mark` | 按 id 修改地址区间 | `id`、`startAddress`、`endAddress` |
| `delete_mark` | 按 id 删除 | `id` |
| `get_status` | 服务状态与 mark 数量 | 无 |
| `submit_marks` | **把全部 mark 提交给宿主程序并停止服务(最终步骤)** | 无 |

## 地址书写规则
- 地址一律用字符串:`"0x1000"`(十六进制)或 `"4096"`(十进制),**不要用 JSON 数字**,避免 64 位地址精度丢失;工具返回的地址也都是 `"0x..."` 字符串。
- 必须满足 `startAddress < endAddress`,否则工具会返回错误。

## 行为约束(必须遵守)
1. **只有当我明确要求"提交"时才调用 `submit_marks`**——它会把当前全部 mark 逐项真实提交给我的程序,并且提交完成后服务会自动停止(最终步骤,不可撤销)。提交前请先用 `list_marks` 与我核对全部条目。
2. **只通过 MCP 工具操作,不要用 curl/浏览器直接访问 HTTP 接口**;尤其禁止访问 `GET /stop`——它会停止服务并提交全部 mark,导致你的 MCP 连接全部断开。
3. mark 只存在于我的程序内存中,我的程序重启后为空,你无法恢复,也不要凭记忆重建。
4. 若工具调用返回 404 或连接失败,先调用 `get_status` 或重新连接(重新 initialize);若始终失败,说明我的插件服务已停止,告诉我即可,**不要**自行尝试启动任何进程。
5. 未得到我的明确指示,不要停止服务、不要批量删除已有 mark。

## 推荐工作流
1. 我给出地址区间 → 你用 `add_mark` 逐条添加(或先 `list_marks` 看现状)。
2. 用 `get_mark` / `list_marks` 与我核对地址是否正确,改错用 `update_mark`,多余用 `delete_mark`。
3. 我确认无误后,你说"提交",你再调用 `submit_marks`,并把返回结果(成功条数/失败条数)原样告诉我;若有失败项,列出失败的 id 和地址。)";
		// 将消息复制到剪贴板
		if (OpenClipboard(nullptr)) {
			EmptyClipboard();
			HGLOBAL hGlobal = GlobalAlloc(GMEM_MOVEABLE, (message.size() + 1) * sizeof(wchar_t));
			if (hGlobal) {
				wchar_t* pGlobal = static_cast<wchar_t*>(GlobalLock(hGlobal));
				if (pGlobal) {
					memcpy(pGlobal, message.c_str(), (message.size() + 1) * sizeof(wchar_t));
					GlobalUnlock(hGlobal);
					SetClipboardData(CF_UNICODETEXT, hGlobal);
				}
				else {
					GlobalFree(hGlobal);
				}
			}
			CloseClipboard();
		}

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
