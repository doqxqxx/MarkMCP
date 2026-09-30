# MarkMCP 使用文档

## 1. 项目说明

MarkMCP 是一个 Windows C++ DLL，内嵌 HTTP 服务，用于通过 REST API 管理内存地址区间（mark）。服务运行在本机回环地址 `127.0.0.1`，默认端口为 `8080`。

> 名称中虽然包含 MCP，但当前实现提供的是 HTTP REST API，并未实现 MCP 标准协议（例如 MCP 的工具发现/调用协议）。普通 HTTP 客户端可以直接调用；若要连接只支持标准 MCP 的客户端，需要另行增加 MCP 适配层。

## 2. 构建

1. 使用 Visual Studio 打开 `plug1/plug1.vcxproj`。
2. 选择 `Debug` 或 `Release` 配置及 `x64` 平台进行构建。
3. 构建产物为 DLL；x64 配置的输出目录由项目设置为解决方案目录下的 `Bin`。

项目配置使用 Windows SDK 和 MSVC 工具集，并链接 Winsock。当前服务仅适用于 Windows。

## 3. 启动与停止

DLL 导出函数为：

```cpp
void GetMarks(pfn_AddMarks pAddMarks);
```

回调定义为：

```cpp
typedef bool (*pfn_AddMarks)(size_t StartAddress, size_t EndAddress);
```

宿主程序加载 DLL 后调用 `GetMarks`，并传入可调用的回调函数。该函数会在调用线程中启动并持续运行 HTTP 服务，直到调用 HTTP 停止接口或服务退出。因此建议在专用线程中调用，并保证回调函数及其依赖在服务整个运行期间有效。

服务固定使用 `127.0.0.1:8080`。如果端口无效、已被占用或服务无法监听，启动会失败。当前导出接口没有提供自定义端口参数。

## 4. 数据模型与地址格式

mark 对象格式：

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
- 建议地址以字符串传输，避免 JavaScript 等客户端将 64 位地址转换为浮点数时丢失精度。读取接口当前返回 JSON 数字。

## 5. HTTP API

所有请求均访问 `http://127.0.0.1:8080`。需要请求体的接口使用 JSON，响应为 JSON。

### 查询全部 mark

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

### 查询单个 mark

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

### 新增 mark

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

### 更新 mark

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

### 删除 mark

```http
DELETE /marks/{id}
```

成功响应：

```json
{"success":true,"id":1}
```

### 查询服务状态

```http
GET /status
```

响应示例：

```json
{"running":true,"done":false,"markCount":1}
```

### 停止服务并提交 mark

```http
GET /stop
```

响应：

```json
{"success":true}
```

该接口会停止 HTTP 服务，并逐项调用宿主提供的 `pfn_AddMarks` 回调，将当前保存的 mark 提交给宿主。若部分回调失败，会在当前工作目录追加写入 `addMarks_errors.log`，并弹出 Windows 错误提示框。请注意：这是有副作用的停止操作，不应作为状态探测接口使用。

## 6. 错误响应

API 错误通常采用以下格式：

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

## 7. 调用示例

使用 `curl` 新增、查询并删除 mark：

```bash
curl -X POST http://127.0.0.1:8080/marks -H "Content-Type: application/json" -d "{\"startAddress\":\"0x1000\",\"endAddress\":\"0x2000\"}"
curl http://127.0.0.1:8080/marks
curl -X DELETE http://127.0.0.1:8080/marks/1
```

## 8. 安全与运行注意事项

- 服务只绑定 `127.0.0.1`，不会监听外部网卡。
- API 没有身份验证；本机其他进程仍可能访问这些接口，请勿将服务暴露到不可信环境。
- mark 只保存在进程内存中，服务重启后不会保留。
- 地址只做 `startAddress < endAddress` 校验；使用者需确保地址在宿主进程的有效范围内。
- 停止服务应调用 `/stop`，以便执行回调提交流程。直接终止宿主进程不会执行该流程。
