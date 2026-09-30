#!/usr/bin/env bash
# mcp_test.sh - plug1.dll MCP 服务器端到端测试
# 前置条件:测试宿主已在后台运行(服务监听 127.0.0.1:8080),回调配置为成功。
# 用法: ./mcp_test.sh

set -u
cd "$(dirname "$0")"

BASE=http://127.0.0.1:8080/mcp
HDRS=/tmp/mcp_headers.txt
BODY=/tmp/mcp_body.txt
PASS=0
FAIL=0

# 失败即记录,不断言退出
check() { # check <说明> <期望> <实际>
    if [ "$2" = "$3" ]; then
        PASS=$((PASS + 1))
        # echo "PASS: $1"
    else
        FAIL=$((FAIL + 1))
        echo "FAIL: $1"
        echo "  expected: $2"
        echo "  actual:   $3"
    fi
}

# POST /mcp;参数: 会话id 版本头 请求体
post() {
    local args=(-s -o "$BODY" -D "$HDRS" -w '%{http_code}' -X POST "$BASE"
        -H 'Content-Type: application/json'
        -H 'Accept: application/json, text/event-stream')
    [ -n "${1:-}" ] && args+=(-H "Mcp-Session-Id: $1")
    [ -n "${2:-}" ] && args+=(-H "MCP-Protocol-Version: $2")
    args+=(--data "${3:-}")
    curl "${args[@]}"
}

# 从响应头取会话 id
get_sid() {
    grep -i '^Mcp-Session-Id:' "$HDRS" | head -1 | sed 's/^[^:]*: *//' | tr -d '\r\n'
}

# 取 JSON 字段: jf <python表达式> [文件]
jf() {
    py mcp_json.py "${2:-$BODY}" "$1" 2>/dev/null
}

get() { # REST GET
    curl -s -o "$BODY" -w '%{http_code}' "http://127.0.0.1:8080$1"
}

echo "=== 1. initialize 握手 ==="
STATUS=$(post "" "" '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25","capabilities":{},"clientInfo":{"name":"mcp-test","version":"0.0.1"}}}')
check "initialize http status" "200" "$STATUS"
SID=$(get_sid)
check "session id issued (32 hex)" "1" "$(echo -n "$SID" | grep -cE '^[0-9a-f]{32}$')"
check "negotiated version echo" "2025-11-25" "$(jf "d['result']['protocolVersion']")"
check "serverInfo name" "MarkMCP" "$(jf "d['result']['serverInfo']['name']")"
check "serverInfo title" "MarkMCP 地址标记服务" "$(jf "d['result']['serverInfo']['title']")"
check "tools capability" "False" "$(jf "d['result']['capabilities']['tools']['listChanged']")"
check "instructions present" "True" "$(jf "bool(d['result'].get('instructions'))")"

echo "=== 2. notifications/initialized ==="
STATUS=$(post "$SID" "2025-11-25" '{"jsonrpc":"2.0","method":"notifications/initialized"}')
check "initialized notification -> 202" "202" "$STATUS"
check "202 has empty body" "0" "$(wc -c < "$BODY")"

echo "=== 3. tools/list ==="
STATUS=$(post "$SID" "2025-11-25" '{"jsonrpc":"2.0","id":2,"method":"tools/list"}')
check "tools/list status" "200" "$STATUS"
check "7 tools" "7" "$(jf "len(d['result']['tools'])")"
check "tool names" "['add_mark', 'delete_mark', 'get_mark', 'get_status', 'list_marks', 'submit_marks', 'update_mark']" \
    "$(jf "sorted(t['name'] for t in d['result']['tools'])")"
check "add_mark required fields" "['startAddress', 'endAddress']" \
    "$(jf "[p for t in d['result']['tools'] if t['name']=='add_mark' for p in t['inputSchema']['required']]")"

echo "=== 4. add_mark (0x 十六进制字符串) ==="
STATUS=$(post "$SID" "2025-11-25" '{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"add_mark","arguments":{"startAddress":"0x1000","endAddress":"0x2000"}}}')
check "add_mark status" "200" "$STATUS"
check "add_mark id" "1" "$(jf "d['result']['structuredContent']['id']")"
check "add_mark hex start" "0x1000" "$(jf "d['result']['structuredContent']['startAddress']")"
check "no isError" "None" "$(jf "d['result'].get('isError')")"

echo "=== 5. add_mark (十进制字符串) ==="
STATUS=$(post "$SID" "2025-11-25" '{"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"add_mark","arguments":{"startAddress":"40960","endAddress":"49152"}}}')
check "second add id" "2" "$(jf "d['result']['structuredContent']['id']")"
check "decimal parsed as hex output" "0xa000" "$(jf "d['result']['structuredContent']['startAddress']")"

echo "=== 6. add_mark 非法区间 -> isError ==="
STATUS=$(post "$SID" "2025-11-25" '{"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"add_mark","arguments":{"startAddress":"0x3000","endAddress":"0x1000"}}}')
check "invalid range isError" "True" "$(jf "d['result'].get('isError')")"
check "invalid range message" "True" "$(jf "'startAddress must be less' in d['result']['content'][0]['text']")"

echo "=== 7. 缺少必填参数 -> -32602 ==="
STATUS=$(post "$SID" "2025-11-25" '{"jsonrpc":"2.0","id":6,"method":"tools/call","params":{"name":"add_mark","arguments":{}}}')
check "missing params error code" "-32602" "$(jf "d['error']['code']")"

echo "=== 8. list_marks ==="
STATUS=$(post "$SID" "2025-11-25" '{"jsonrpc":"2.0","id":7,"method":"tools/call","params":{"name":"list_marks","arguments":{}}}')
check "list count" "2" "$(jf "d['result']['structuredContent']['count']")"
check "list first id" "1" "$(jf "d['result']['structuredContent']['marks'][0]['id']")"

echo "=== 9. get_mark / get_mark 不存在 ==="
STATUS=$(post "$SID" "2025-11-25" '{"jsonrpc":"2.0","id":8,"method":"tools/call","params":{"name":"get_mark","arguments":{"id":1}}}')
check "get_mark start" "0x1000" "$(jf "d['result']['structuredContent']['startAddress']")"
STATUS=$(post "$SID" "2025-11-25" '{"jsonrpc":"2.0","id":9,"method":"tools/call","params":{"name":"get_mark","arguments":{"id":99}}}')
check "get_mark missing isError" "True" "$(jf "d['result'].get('isError')")"
STATUS=$(post "$SID" "2025-11-25" '{"jsonrpc":"2.0","id":10,"method":"tools/call","params":{"name":"get_mark","arguments":{"id":-1}}}')
check "negative id -> -32602" "-32602" "$(jf "d['error']['code']")"

echo "=== 10. update_mark / delete_mark ==="
STATUS=$(post "$SID" "2025-11-25" '{"jsonrpc":"2.0","id":11,"method":"tools/call","params":{"name":"update_mark","arguments":{"id":1,"startAddress":"0x4000","endAddress":"0x5000"}}}')
check "update start" "0x4000" "$(jf "d['result']['structuredContent']['startAddress']")"
STATUS=$(post "$SID" "2025-11-25" '{"jsonrpc":"2.0","id":12,"method":"tools/call","params":{"name":"delete_mark","arguments":{"id":2}}}')
check "delete ok" "True" "$(jf "d['result']['structuredContent']['deleted']")"
STATUS=$(post "$SID" "2025-11-25" '{"jsonrpc":"2.0","id":13,"method":"tools/call","params":{"name":"delete_mark","arguments":{"id":2}}}')
check "delete again isError" "True" "$(jf "d['result'].get('isError')")"

echo "=== 11. get_status / ping ==="
STATUS=$(post "$SID" "2025-11-25" '{"jsonrpc":"2.0","id":14,"method":"tools/call","params":{"name":"get_status","arguments":{}}}')
check "status markCount" "1" "$(jf "d['result']['structuredContent']['markCount']")"
check "status running" "True" "$(jf "d['result']['structuredContent']['running']")"
STATUS=$(post "$SID" "2025-11-25" '{"jsonrpc":"2.0","id":15,"method":"ping"}')
check "ping result empty object" "{}" "$(jf "d['result']")"

echo "=== 12. REST API 回归 ==="
STATUS=$(get /marks)
check "REST GET /marks" "200" "$STATUS"
check "REST marks count" "1" "$(jf "len(d)")"
STATUS=$(curl -s -o "$BODY" -w '%{http_code}' -X POST http://127.0.0.1:8080/marks -H "Content-Type: application/json" --data '{"startAddress":"0x7100","endAddress":"0x7200"}')
check "REST POST /marks 201" "201" "$STATUS"
STATUS=$(get /status)
check "REST /status markCount" "2" "$(jf "d['markCount']")"

echo "=== 13. 协议错误处理 ==="
STATUS=$(post "$SID" "2025-11-25" '{bad json')
check "parse error -> 400/-32700" "400|-32700" "$STATUS|$(jf "d['error']['code']")"
STATUS=$(post "" "" '{"jsonrpc":"2.0","id":17,"method":"tools/list"}')
check "no session -> 400/-32600" "400|-32600" "$STATUS|$(jf "d['error']['code']")"
STATUS=$(post "deadbeefdeadbeefdeadbeefdeadbeef" "" '{"jsonrpc":"2.0","id":18,"method":"tools/list"}')
check "bad session -> 404" "404" "$STATUS"
STATUS=$(post "$SID" "1999-01-01" '{"jsonrpc":"2.0","id":19,"method":"tools/list"}')
check "unsupported protocol version -> 400" "400" "$STATUS"
STATUS=$(post "$SID" "2025-11-25" '{"jsonrpc":"2.0","id":20,"method":"foo/bar"}')
check "unknown method -> -32601" "-32601" "$(jf "d['error']['code']")"
STATUS=$(post "$SID" "2025-11-25" '{"jsonrpc":"2.0","id":21,"method":"tools/call","params":{"name":"no_such_tool","arguments":{}}}')
check "unknown tool -> -32602" "-32602|Unknown tool: no_such_tool" "$(jf "d['error']['code']")|$(jf "d['error']['message']")"
STATUS=$(post "$SID" "2025-11-25" '{"jsonrpc":"2.0","id":22,"method":"tools/call","params":{"name":"get_mark","arguments":{"id":"abc"}}}')
check "bad id type -> -32602" "-32602" "$(jf "d['error']['code']")"
STATUS=$(post "$SID" "2025-11-25" '[{"jsonrpc":"2.0","id":23,"method":"tools/list"}]')
check "batch on 2025-11-25 -> 400/-32600" "400|-32600" "$STATUS|$(jf "d['error']['code']")"

echo "=== 15. Origin 校验 ==="
STATUS=$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE" -H 'Origin: http://evil.example.com' -H 'Content-Type: application/json' --data '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}')
check "foreign origin -> 403" "403" "$STATUS"
STATUS=$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE" -H 'Origin: http://localhost:3000' -H 'Content-Type: application/json' --data '{"jsonrpc":"2.0","id":24,"method":"initialize","params":{"protocolVersion":"2025-11-25"}}')
check "localhost origin allowed" "200" "$STATUS"

echo "=== 16. GET SSE 流 ==="
curl -s -N -D /tmp/mcp_sse_headers.txt --max-time 18 -H "Mcp-Session-Id: $SID" "$BASE" > /tmp/mcp_sse.txt 2>/dev/null || true
check "SSE content-type" "1" "$(grep -ci 'content-type: text/event-stream' /tmp/mcp_sse_headers.txt)"
check "SSE retry field" "1" "$(grep -c '^retry: 5000' /tmp/mcp_sse.txt)"
check "SSE connected comment" "1" "$(grep -c '^: connected' /tmp/mcp_sse.txt)"
check "SSE keep-alive (15s)" "1" "$(grep -c '^: keep-alive' /tmp/mcp_sse.txt)"
STATUS=$(curl -s -o /dev/null -w '%{http_code}' --max-time 2 "$BASE" 2>/dev/null)
check "GET without session -> 400" "400" "$STATUS"

echo "=== 17. DELETE 会话终止 ==="
STATUS=$(curl -s -o /dev/null -w '%{http_code}' -X DELETE -H "Mcp-Session-Id: $SID" "$BASE")
check "DELETE -> 200" "200" "$STATUS"
STATUS=$(post "$SID" "2025-11-25" '{"jsonrpc":"2.0","id":25,"method":"tools/list"}')
check "terminated session -> 404" "404" "$STATUS"

echo "=== 18. 重新 initialize(会话轮换) ==="
STATUS=$(post "" "" '{"jsonrpc":"2.0","id":26,"method":"initialize","params":{"protocolVersion":"2025-06-18"}}')
check "re-initialize status" "200" "$STATUS"
SID2=$(get_sid)
check "new session id differs" "1" "$([ -n "$SID2" ] && [ "$SID2" != "$SID" ] && echo 1 || echo 0)"
check "older version negotiated" "2025-06-18" "$(jf "d['result']['protocolVersion']")"
STATUS=$(post "$SID2" "2025-06-18" '{"jsonrpc":"2.0","id":27,"method":"ping"}')
check "old version session ping" "200" "$STATUS"

echo "=== 18. submit_marks(最终步骤:提交并停止服务) ==="
STATUS=$(post "$SID2" "2025-06-18" '{"jsonrpc":"2.0","id":28,"method":"tools/call","params":{"name":"submit_marks","arguments":{}}}')
check "submit status (响应在停服前送达)" "200" "$STATUS"
check "submit total (id1 + REST 新增)" "2" "$(jf "d['result']['structuredContent']['total']")"
check "submit failed" "0" "$(jf "d['result']['structuredContent']['failed']")"
check "serviceStopped flag" "True" "$(jf "d['result']['structuredContent']['serviceStopped']")"
check "submit text mentions stop" "True" "$(jf "'service stopped' in d['result']['content'][0]['text']")"
check "submit not error" "None" "$(jf "d['result'].get('isError')")"

echo "=== 19. 提交后服务已自动停止 ==="
STATUS=$(curl -s -o /dev/null -w '%{http_code}' --max-time 2 http://127.0.0.1:8080/status 2>/dev/null)
check "REST /status -> connection refused" "000" "$STATUS"
STATUS=$(post "$SID2" "2025-06-18" '{"jsonrpc":"2.0","id":29,"method":"ping"}')
check "MCP endpoint -> connection refused" "000" "$STATUS"

echo
echo "RESULT: PASS=$PASS FAIL=$FAIL"
[ "$FAIL" = "0" ]
