#!/usr/bin/env python3
"""MCP server: SSH bridge to the RK3588S board.

Claude Code 通过标准输入/输出与本服务通信（JSON-RPC 2.0 over stdio）。
本服务将工具调用转发为 SSH / SCP 命令，让 Claude Code 能直接操作板子。

使用方式（.mcp.json 中已配置）：
  python3 tools/board_mcp.py

板子的 SSH 地址是本机/局域网相关的配置，不进 git：复制
tools/board_config.example.json 为 tools/board_config.json 并填入实际地址
（按优先级排列的列表，前面的连不上会自动 failover 到下一个）。
"""
import json
import sys
import subprocess
import os

_CONFIG_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "board_config.json")


def _load_hosts() -> list:
    if not os.path.exists(_CONFIG_PATH):
        raise SystemExit(
            f"missing {_CONFIG_PATH} — copy board_config.example.json to board_config.json "
            "and fill in your board's SSH host(s)"
        )
    with open(_CONFIG_PATH) as f:
        cfg = json.load(f)
    hosts = cfg.get("hosts")
    if not hosts:
        raise SystemExit(f"{_CONFIG_PATH} must define a non-empty 'hosts' list")
    return hosts


# 按优先级排列的 SSH 地址列表（如 有线 IP, WiFi 备用 IP），逐个尝试，
# 连接层失败（无路由/拒绝/超时）才会 failover 到下一个，不是每次都尝试全部。
HOSTS = _load_hosts()

# BatchMode=yes：禁止交互式密码提示（需提前配置 SSH 免密）
# ConnectTimeout=5：连接超时 5s，保证主备切换不会等太久
SSH_OPTS = ["-o", "BatchMode=yes", "-o", "ConnectTimeout=5"]


def _run(args: list, timeout: int) -> subprocess.CompletedProcess | None:
    """运行一次 ssh/scp，连接层异常（超时/无法解析等）时返回 None 交给上层 failover。"""
    try:
        return subprocess.run(args, capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return None


def ssh(cmd: str, timeout: int = 30) -> dict:
    """在板子上执行 shell 命令，按 HOSTS 顺序尝试，返回 stdout/stderr/returncode/host。"""
    last = None
    for host in HOSTS:
        r = _run(["ssh"] + SSH_OPTS + [host, cmd], timeout=timeout)
        # returncode 255 是 ssh 连接层失败（拒绝/超时/无路由），区别于远程命令本身的非 0 返回值
        if r is not None and r.returncode != 255:
            return {"stdout": r.stdout, "stderr": r.stderr, "returncode": r.returncode, "host": host}
        last = r
    if last is None:
        return {"stdout": "", "stderr": "ssh timed out on all hosts", "returncode": 255, "host": HOSTS[-1]}
    return {"stdout": last.stdout, "stderr": last.stderr, "returncode": last.returncode, "host": HOSTS[-1]}


def scp_upload(local: str, remote: str, timeout: int = 60) -> dict:
    """将本地文件上传到板子指定路径，按 HOSTS 顺序尝试。"""
    last = None
    for host in HOSTS:
        r = _run(["scp"] + SSH_OPTS + [local, f"{host}:{remote}"], timeout=timeout)
        if r is not None and r.returncode != 255:
            return {"stdout": r.stdout, "stderr": r.stderr, "returncode": r.returncode, "host": host}
        last = r
    if last is None:
        return {"stdout": "", "stderr": "scp timed out on all hosts", "returncode": 255, "host": HOSTS[-1]}
    return {"stdout": last.stdout, "stderr": last.stderr, "returncode": last.returncode, "host": HOSTS[-1]}


# MCP 工具定义：Claude Code 通过 tools/list 获取此列表，按 name 调用对应工具
TOOLS = [
    {
        "name": "board_exec",
        "description": "Run a shell command on the RK3588S board via SSH. Returns stdout, stderr, and exit code.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "command": {"type": "string", "description": "Shell command to run on the board"},
                "timeout": {"type": "integer", "description": "Timeout in seconds (default 30)", "default": 30}
            },
            "required": ["command"]
        }
    },
    {
        "name": "board_upload",
        "description": "Upload a local file to the board via scp.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "local_path": {"type": "string", "description": "Absolute path of the local file to upload"},
                "remote_path": {"type": "string", "description": "Destination path on the board (e.g. ~/AlertGateway/AlertGateway)"}
            },
            "required": ["local_path", "remote_path"]
        }
    },
    {
        "name": "board_log",
        "description": "Read the AlertGateway log file (/tmp/ag.log) from the board.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "lines": {"type": "integer", "description": "Number of tail lines to return (default 50)", "default": 50}
            }
        }
    }
]


def call_tool(name: str, args: dict) -> str:
    """将 MCP 工具调用分发到对应的 SSH/SCP 操作。"""
    if name == "board_exec":
        cmd = args["command"]
        timeout = args.get("timeout", 30)
        r = ssh(cmd, timeout=timeout)
        parts = []
        if r["host"] != HOSTS[0]: parts.append(f"(via fallback host {r['host']})")
        if r["stdout"]: parts.append(f"stdout:\n{r['stdout'].rstrip()}")
        if r["stderr"]: parts.append(f"stderr:\n{r['stderr'].rstrip()}")
        parts.append(f"exit code: {r['returncode']}")
        return "\n".join(parts)

    elif name == "board_upload":
        local = args["local_path"]
        remote = args["remote_path"]
        if not os.path.exists(local):
            return f"Error: local file not found: {local}"
        r = scp_upload(local, remote)
        if r["returncode"] == 0:
            suffix = f" (via fallback host {r['host']})" if r["host"] != HOSTS[0] else ""
            return f"Uploaded {local} → {r['host']}:{remote}{suffix}"
        return f"scp failed (exit {r['returncode']}):\n{r['stderr']}"

    elif name == "board_log":
        # AlertGateway 运行时将 stdout/stderr 重定向到 /tmp/ag.log
        lines = args.get("lines", 50)
        r = ssh(f"tail -{lines} /tmp/ag.log 2>/dev/null || echo '(log empty)'")
        return r["stdout"] or r["stderr"] or "(no output)"

    else:
        return f"Unknown tool: {name}"


def respond(msg: dict):
    """向 stdout 写一行 JSON-RPC 响应（MCP stdio 协议要求每条消息独占一行）。"""
    sys.stdout.write(json.dumps(msg) + "\n")
    sys.stdout.flush()


def main():
    """主循环：从 stdin 逐行读取 JSON-RPC 请求并响应。"""
    for raw in sys.stdin:
        raw = raw.strip()
        if not raw:
            continue
        try:
            req = json.loads(raw)
        except json.JSONDecodeError:
            continue

        method = req.get("method", "")
        req_id = req.get("id")
        params = req.get("params", {})

        if method == "initialize":
            # 握手：返回协议版本和服务器信息
            respond({"jsonrpc": "2.0", "id": req_id, "result": {
                "protocolVersion": "2024-11-05",
                "capabilities": {"tools": {}},
                "serverInfo": {"name": "board-mcp", "version": "1.0"}
            }})

        elif method == "notifications/initialized":
            pass  # 通知类消息不需要响应

        elif method == "tools/list":
            respond({"jsonrpc": "2.0", "id": req_id, "result": {"tools": TOOLS}})

        elif method == "tools/call":
            tool_name = params.get("name", "")
            tool_args = params.get("arguments", {})
            try:
                result_text = call_tool(tool_name, tool_args)
            except subprocess.TimeoutExpired:
                result_text = "Error: command timed out"
            except Exception as e:
                result_text = f"Error: {e}"
            respond({"jsonrpc": "2.0", "id": req_id, "result": {
                "content": [{"type": "text", "text": result_text}]
            }})

        elif req_id is not None:
            # 未知方法，返回标准 JSON-RPC 错误码
            respond({"jsonrpc": "2.0", "id": req_id, "error": {
                "code": -32601, "message": f"Method not found: {method}"
            }})


if __name__ == "__main__":
    main()
