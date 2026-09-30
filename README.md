# MarkMCP 使用文档

## 1. 项目说明

MarkMCP 是一个 Windows C++ DLL，内嵌 HTTP 服务，用于通过 API 管理内存地址区间（mark）。服务运行在本机回环地址 `127.0.0.1`，默认端口为 `8080`。

服务同时提供两套接口：

- **标准 MCP（Model Context Protocol）接口**：端点 `http://127.0.0.1:8080/mcp`，使用 Streamable HTTP 传输，可被任何支持 MCP 的客户端（如 Claude Desktop、Cursor、ZCode 等）直接连接，把 mark 管理能力以 MCP 工具的形式暴露给 AI 模型。
- **HTTP REST API**：`/marks`、`/status`、`/stop` 等端点，供普通 HTTP 客户端直接调用。

## 2. 构建

1. 使用 Visual Studio 打开 `plug1/plug1.vcxproj`（或 `plug1.sln`）。
2. 选择 `Debug` 或 `Release` 配置及 `x64` 平台进行构建。
3. 构建产物为 DLL；x64 配置的输出目录由项目设置为解决方案目录下的 `Bin`。

项目配置使用 Windows SDK 和 MSVC 工具集，链接 Winsock 与 user32，统一使用 `/utf-8` 编译选项。当前服务仅适用于 Windows。

## 3. 启动与停止

DLL 导出函数为：

```cpp
void GetMarks(pfn_AddMarks pAddMarks);
```

回调定义为：

```cpp
typedef bool (*pfn_AddMarks)(size_t StartAddress, size_t EndAddress);
```

宿主程序加载 DLL 后调用 `GetMarks`，并传入可调用的回调函数。该函数会在调用线程中阻塞运行 HTTP/MCP 服务，直到服务被停止（见下方停止方式），随后返回。因此建议在专用线程中调用，并保证回调函数及其依赖在服务整个运行期间有效。

服务固定使用 `127.0.0.1:8080`。如果端口无效或已被占用，启动会失败（弹窗提示）。当前导出接口没有提供自定义端口参数。

停止方式（两者都会先逐项提交全部 mark，再停止服务，`GetMarks` 随之返回、宿主继续运行）：

- MCP 工具 `submit_marks`：提交并停止服务（MCP 流程的最终步骤）。
- `GET /stop`（REST）：提交并停止整个 HTTP/MCP 服务。

## 4. 数据模型与地址格式

mark 对象格式（REST 接口）：

```json
{
  "id": 1,
  "startAddress": 4096,
  "endAddress": 8192
}
```

- `id`：服务分配的递增标识。
- `startAddress`、`endAddress`：地址范围，要求 `startAddress < endAddress`。
- 写入接口的地址可使用 JSON 整数，或使用字符串。字符串支持十进制及带 `0x` 前缀的十六进制，例如 `"0x1000"`。
- 建议地址以字符串传输，避免 JavaScript 等客户端将 64 位地址转换为浮点数时丢失精度。REST 读取接口当前返回 JSON 数字；**MCP 工具的输入输出统一使用 `"0x..."` 十六进制字符串**，无精度问题。

## 5. MCP 接口

### 5.1 端点与传输

| 项目 | 值 |
| --- | --- |
| 端点 URL | `http://127.0.0.1:8080/mcp` |
| 传输方式 | Streamable HTTP（MCP 标准 HTTP 传输） |
| 支持的协议版本 | `2024-11-05`、`2025-03-26`、`2025-06-18`、`2025-11-25`（初始化时协商） |
| 已实现能力 | `tools`（工具列表为静态，无变更通知） |

MCP 客户端配置示例（`mcpServers` 配置格式，客户端各有差异）：

```json
{
  "mcpServers": {
    "mark-mcp": {
      "type": "http",
      "url": "http://127.0.0.1:8080/mcp"
    }
  }
}
```

仅支持 stdio 传输的客户端，可借助 `mcp-remote`、`supergateway` 等桥接工具将本 HTTP 端点转换为 stdio。

### 5.2 会话管理

- 客户端发送 `initialize` 请求后，服务端在响应头 `Mcp-Session-Id` 中返回会话 ID（32 位十六进制）。
- 之后的每个请求都必须携带 `Mcp-Session-Id` 头：缺失返回 `400`，会话无效/已终止返回 `404`（此时客户端应重新 initialize）。
- 客户端可发送 `DELETE /mcp`（携带会话头）显式终止会话。
- 后续请求建议携带 `MCP-Protocol-Version` 头；出现不支持的版本时服务端返回 `400`。
- `GET /mcp` 打开 SSE 长连接（`text/event-stream`），当前实现不主动推送业务消息，仅发送 `retry` 建议与 keep-alive 心跳注释。

### 5.3 工具列表

| 工具 | 说明 | 参数 |
| --- | --- | --- |
| `add_mark` | 新增一个 mark | `startAddress`、`endAddress`（字符串：十进制或 `0x` 十六进制；也接受整数） |
| `list_marks` | 列出全部 mark | 无 |
| `get_mark` | 按 id 查询单个 mark | `id`（非负整数） |
| `update_mark` | 按 id 修改地址区间 | `id`、`startAddress`、`endAddress` |
| `delete_mark` | 按 id 删除 | `id` |
| `get_status` | 查询运行状态与 mark 数量 | 无 |
| `submit_marks` | **把全部 mark 提交给宿主程序并停止服务(最终步骤)** | 无 |

行为约定：

- 地址校验失败、mark 不存在等业务错误以 `isError: true` 的工具结果返回，便于模型理解并自我修正；未知工具、缺少必填参数等协议层错误以 JSON-RPC `-32602` 错误返回。
- 工具结果同时包含 `content`（文本）与 `structuredContent`（结构化对象）；地址统一以 `"0x..."` 字符串表示。
- `submit_marks` 逐项调用宿主回调提交全部 mark，结果包含每个 mark 的 `success` 与 `serviceStopped` 字段；任一失败则整体 `isError: true`。**提交完成后服务会自动停止**——这是最终步骤，不可再修改或重复提交，提交前请先用 `list_marks` 核对（该工具不弹窗、不写日志）。
- REST `GET /stop` 同样是「提交并停止」（见 6.7 节），区别是失败时会追加写 `addMarks_errors.log` 并弹窗提示。

### 5.4 调用示例（curl）

初始化握手并保存会话 ID：

```bash
curl -s -D headers.txt -X POST http://127.0.0.1:8080/mcp \
  -H "Content-Type: application/json" \
  -H "Accept: application/json, text/event-stream" \
  -d '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25","capabilities":{},"clientInfo":{"name":"demo","version":"1.0"}}}'
SID=$(grep -i '^Mcp-Session-Id:' headers.txt | sed 's/[^:]*: *//' | tr -d '\r')
```

发送初始化完成通知，然后新增并查询 mark：

```bash
curl -s -X POST http://127.0.0.1:8080/mcp \
  -H "Content-Type: application/json" -H "Mcp-Session-Id: $SID" \
  -d '{"jsonrpc":"2.0","method":"notifications/initialized"}'

curl -s -X POST http://127.0.0.1:8080/mcp \
  -H "Content-Type: application/json" -H "Mcp-Session-Id: $SID" \
  -d '{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"add_mark","arguments":{"startAddress":"0x1000","endAddress":"0x2000"}}}'

curl -s -X POST http://127.0.0.1:8080/mcp \
  -H "Content-Type: application/json" -H "Mcp-Session-Id: $SID" \
  -d '{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"list_marks","arguments":{}}}'

curl -s -X POST http://127.0.0.1:8080/mcp \
  -H "Content-Type: application/json" -H "Mcp-Session-Id: $SID" \
  -d '{"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"submit_marks","arguments":{}}}'
```

## 6. HTTP REST API

所有请求均访问 `http://127.0.0.1:8080`。需要请求体的接口使用 JSON，响应为 JSON。

### 6.1 查询全部 mark

```http
GET /marks
```

成功响应：`200 OK`

```json
[
  {"id":1,"startAddress":4096,"endAddress":8192}
]
```

没有 mark 时返回空数组 `[]`。

### 6.2 查询单个 mark

```http
GET /marks/{id}
```

成功响应示例：

```json
{"id":1,"startAddress":4096,"endAddress":8192}
```

不存在时返回 `404`：

```json
{"success":false,"error":"mark not found"}
```

### 6.3 新增 mark

```http
POST /marks
Content-Type: application/json
```

请求体：

```json
{"startAddress":"0x1000","endAddress":"0x2000"}
```

成功返回 `201 Created`：

```json
{"success":true,"id":1}
```

### 6.4 更新 mark

```http
PUT /marks/{id}
Content-Type: application/json
```

请求体：

```json
{"startAddress":"0x3000","endAddress":"0x4000"}
```

成功响应：

```json
{"success":true,"id":1}
```

### 6.5 删除 mark

```http
DELETE /marks/{id}
```

成功响应：

```json
{"success":true,"id":1}
```

### 6.6 查询服务状态

```http
GET /status
```

响应示例：

```json
{"running":true,"done":false,"markCount":1}
```

### 6.7 停止服务并提交 mark

```http
GET /stop
```

响应：

```json
{"success":true}
```

该接口会停止 HTTP/MCP 服务（包括所有 MCP 会话与 SSE 连接），并逐项调用宿主提供的 `pfn_AddMarks` 回调，将当前保存的 mark 提交给宿主。若部分回调失败，会在当前工作目录追加写入 `addMarks_errors.log`，并弹出 Windows 错误提示框。请注意：这是有副作用的停止操作，不应作为状态探测接口使用。

## 7. 错误响应

REST API 错误通常采用以下格式：

```json
{"success":false,"error":"错误说明"}
```

常见状态码：

| HTTP 状态码 | 含义 |
| --- | --- |
| `400` | JSON 格式错误、缺少地址字段、ID 无效或地址范围不合法 |
| `404` | 指定 ID 的 mark 不存在 |
| `201` | mark 创建成功 |
| `200` | 请求成功 |

地址范围非法时错误内容为 `invalid address range`；缺少地址字段时为 `startAddress and endAddress are required`；JSON 无法解析时为 `invalid json`。

MCP 端点的错误遵循 JSON-RPC 2.0 与 MCP 规范：`-32700` 解析错误、`-32600` 无效请求、`-32601` 方法不存在、`-32602` 参数错误，以及 `isError` 工具结果。

## 8. REST 调用示例

使用 `curl` 新增、查询并删除 mark：

```bash
curl -X POST http://127.0.0.1:8080/marks -H "Content-Type: application/json" -d "{\"startAddress\":\"0x1000\",\"endAddress\":\"0x2000\"}"
curl http://127.0.0.1:8080/marks
curl -X DELETE http://127.0.0.1:8080/marks/1
```

## 9. 自动化测试

`test/` 目录包含端到端测试工具：

- `mcp_test_host.cpp`：模拟宿主进程，加载 `plug1.dll` 并调用 `GetMarks`（阻塞直至服务停止后返回）；第二个参数指定宿主回调的返回值（`1` 成功，`0` 失败）。
- `build_and_run_host.bat`：调用 MSVC 编译 DLL 与测试宿主到 `test/build/`（自动定位 VS 环境，当前为 VS 2026 / v145）。
- `mcp_test.sh`：对 initialize/会话管理/7 个工具/协议错误/Origin 校验/SSE 流/DELETE 会话/REST 回归/提交后自动停服等 66 项断言的测试套件。

运行方式（Git Bash）：

```bash
cd test
cmd //c build_and_run_host.bat          # 编译
./build/mcp_test_host.exe "$(cygpath -m "$PWD/build/plug1.dll")" 1 &   # 启动宿主(回调成功)
sleep 2
bash mcp_test.sh                        # 运行测试套件(需全新服务状态)
```

注意：`mcp_test.sh` 依赖全新服务状态（mark 计数与 id 断言），且末尾的 `submit_marks` 会停止服务——重跑前需重启宿主。若 DLL 内启用了加载确认弹窗，需点击后服务才会启动。

## 10. 安全与运行注意事项

- 服务只绑定 `127.0.0.1`，不会监听外部网卡。
- API 没有身份验证；本机其他进程仍可能访问这些接口，请勿将服务暴露到不可信环境。
- MCP 端点实现了规范要求的 `Origin` 校验：浏览器发送的非回环地址 Origin 返回 `403`，以缓解 DNS rebinding 攻击。
- mark 只保存在进程内存中，服务重启后不会保留。
- 地址只做 `startAddress < endAddress` 校验；使用者需确保地址在宿主进程的有效范围内。
- 停止服务请使用 MCP `submit_marks` 或 REST `/stop`（两者都会先提交全部 mark 再停止）。直接终止宿主进程不会执行提交流程。

## 11. 许可证

本项目采用 [GPL-3.0](LICENSE)（GNU 通用公共许可证第 3 版）发布：你可以自由使用、修改和分发本项目，但基于本项目的衍生作品必须同样以 GPL-3.0 开源。
