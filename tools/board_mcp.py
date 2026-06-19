#!/usr/bin/env python3
"""MCP server: SSH bridge to the RK3588S board at 192.168.0.200.

Claude Code 通过标准输入/输出与本服务通信（JSON-RPC 2.0 over stdio）。
本服务将工具调用转发为 SSH / SCP 命令，让 Claude Code 能直接操作板子。

使用方式（.mcp.json 中已配置）：
  python3 tools/board_mcp.py
"""
import json
import sys
import subprocess
import os

# 板子 SSH 地址：RK3588S（firefly 开发板），IP 固定为 192.168.0.200
BOARD = "firefly@192.168.0.200"

# BatchMode=yes：禁止交互式密码提示（需提前配置 SSH 免密）
# ConnectTimeout=10：连接超时 10s，避免板子掉线时 Claude Code 长时间卡住
SSH_OPTS = ["-o", "BatchMode=yes", "-o", "ConnectTimeout=10"]


def ssh(cmd: str, timeout: int = 30) -> dict:
    """在板子上执行 shell 命令，返回 stdout/stderr/returncode。"""
    r = subprocess.run(
        ["ssh"] + SSH_OPTS + [BOARD, cmd],
        capture_output=True, text=True, timeout=timeout
    )
    return {"stdout": r.stdout, "stderr": r.stderr, "returncode": r.returncode}


def scp_upload(local: str, remote: str, timeout: int = 60) -> dict:
    """将本地文件上传到板子指定路径。"""
    r = subprocess.run(
        ["scp"] + SSH_OPTS + [local, f"{BOARD}:{remote}"],
        capture_output=True, text=True, timeout=timeout
    )
    return {"stdout": r.stdout, "stderr": r.stderr, "returncode": r.returncode}


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
            return f"Uploaded {local} → {BOARD}:{remote}"
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
