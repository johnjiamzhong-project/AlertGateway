# Firefly RK3588S HDMI / Sunshine 自动切换方案

## 背景

板子默认用 dummy 虚拟显示驱动 + Sunshine 串流给 Moonlight 使用。
插 HDMI 显示器时需要切换到真实显示输出，拔掉后自动切回 Sunshine 串流模式。

---

## 环境

- 板子：Firefly ROC-RK3588S-PC，Ubuntu，IP `192.168.1.200`
- 用户目录：`/home/firefly`
- 显示管理器：lightdm
- 虚拟显示配置：`/etc/X11/xorg.conf.d/10-dummy.conf` 和 `99-dummy.conf`
- Sunshine 配置：`~/.config/sunshine/sunshine.conf`

---

## 文件清单

| 文件 | 说明 |
|------|------|
| `~/sunshine.sh` | 手动控制脚本（start/stop/status/pair/dummy/hdmi） |
| `~/hdmi-switch.sh` | udev/开机触发的自动切换脚本 |
| `/etc/udev/rules.d/99-hdmi-switch.rules` | udev 热插拔规则 |
| `/etc/systemd/system/hdmi-init.service` | 开机初始检测服务 |

---

## Step 1：手动控制脚本 sunshine.sh

```bash
cat > ~/sunshine.sh << 'EOF'
#!/bin/bash
SUNSHINE_USER="sunshine"
SUNSHINE_PASS="sunshine"
DUMMY_CONF_10="/etc/X11/xorg.conf.d/10-dummy.conf"
DUMMY_CONF_99="/etc/X11/xorg.conf.d/99-dummy.conf"
DUMMY_BACKUP_10="/etc/X11/xorg.conf.d/10-dummy.conf.bak"
DUMMY_BACKUP_99="/etc/X11/xorg.conf.d/99-dummy.conf.bak"

case "$1" in
    start)
        rm -f /tmp/sunshine.log /tmp/sunshine.pid 2>/dev/null
        sudo rm -f /tmp/sunshine.log /tmp/sunshine.pid 2>/dev/null
        export DISPLAY=:0
        nohup sunshine > /tmp/sunshine.log 2>&1 &
        echo $! > /tmp/sunshine.pid
        sleep 2
        if pgrep sunshine > /dev/null; then
            echo "Sunshine started, pid: $(cat /tmp/sunshine.pid)"
        else
            echo "Sunshine failed to start, check /tmp/sunshine.log"
        fi
        ;;
    stop)
        pkill sunshine
        rm -f /tmp/sunshine.pid
        echo "Sunshine stopped"
        ;;
    status)
        if pgrep sunshine > /dev/null; then
            echo "Sunshine is running"
            ss -tlnp | grep 47990
        else
            echo "Sunshine is not running"
        fi
        if [ -f "$DUMMY_CONF_10" ]; then
            echo "Display mode: dummy (Sunshine streaming)"
        else
            echo "Display mode: HDMI"
        fi
        ;;
    pair)
        if [ -z "$2" ]; then
            echo "Usage: $0 pair <PIN码>"
        else
            curl -k -u "${SUNSHINE_USER}:${SUNSHINE_PASS}" \
              -X POST \
              -H "Content-Type: application/json" \
              -d "{\"pin\":\"$2\"}" \
              https://localhost:47990/api/pin
        fi
        ;;
    dummy)
        echo "Switching to dummy display mode..."
        [ ! -f "$DUMMY_CONF_10" ] && [ -f "$DUMMY_BACKUP_10" ] && sudo mv "$DUMMY_BACKUP_10" "$DUMMY_CONF_10"
        [ ! -f "$DUMMY_CONF_99" ] && [ -f "$DUMMY_BACKUP_99" ] && sudo mv "$DUMMY_BACKUP_99" "$DUMMY_CONF_99"
        sudo systemctl restart lightdm
        sleep 3
        $0 start
        ;;
    hdmi)
        echo "Switching to HDMI display mode..."
        $0 stop
        [ -f "$DUMMY_CONF_10" ] && sudo mv "$DUMMY_CONF_10" "$DUMMY_BACKUP_10"
        [ -f "$DUMMY_CONF_99" ] && sudo mv "$DUMMY_CONF_99" "$DUMMY_BACKUP_99"
        sudo systemctl restart lightdm
        echo "Switched to HDMI mode"
        ;;
    *)
        echo "Usage: $0 {start|stop|status|pair <PIN码>|dummy|hdmi}"
        ;;
esac
EOF
chmod +x ~/sunshine.sh
```

---

## Step 2：自动切换脚本 hdmi-switch.sh

```bash
cat > ~/hdmi-switch.sh << 'EOF'
#!/bin/bash
DUMMY_CONF_10="/etc/X11/xorg.conf.d/10-dummy.conf"
DUMMY_BACKUP_10="/etc/X11/xorg.conf.d/10-dummy.conf.bak"
DUMMY_CONF_99="/etc/X11/xorg.conf.d/99-dummy.conf"
DUMMY_BACKUP_99="/etc/X11/xorg.conf.d/99-dummy.conf.bak"

sleep 2  # 等待 HDMI 信号稳定

HDMI_STATUS=$(cat /sys/class/drm/*/status 2>/dev/null | grep -c "^connected")

if [ "$HDMI_STATUS" -gt 0 ]; then
    # 插入 HDMI，切换到真实显示
    [ -f "$DUMMY_CONF_10" ] && mv "$DUMMY_CONF_10" "$DUMMY_BACKUP_10"
    [ -f "$DUMMY_CONF_99" ] && mv "$DUMMY_CONF_99" "$DUMMY_BACKUP_99"
    pkill sunshine
    systemctl restart lightdm
else
    # 拔出 HDMI，切回 dummy + Sunshine
    [ -f "$DUMMY_BACKUP_10" ] && mv "$DUMMY_BACKUP_10" "$DUMMY_CONF_10"
    [ -f "$DUMMY_BACKUP_99" ] && mv "$DUMMY_BACKUP_99" "$DUMMY_CONF_99"
    systemctl restart lightdm
    sleep 3
    su - firefly -c "DISPLAY=:0 nohup sunshine > /tmp/sunshine.log 2>&1 &"
fi
EOF
chmod +x ~/hdmi-switch.sh
```

---

## Step 3：udev 热插拔规则

```bash
sudo tee /etc/udev/rules.d/99-hdmi-switch.rules << 'EOF'
ACTION=="change", SUBSYSTEM=="drm", RUN+="/home/firefly/hdmi-switch.sh"
EOF

sudo udevadm control --reload-rules
```

---

## Step 4：开机初始检测服务

```bash
sudo tee /etc/systemd/system/hdmi-init.service << 'EOF'
[Unit]
Description=HDMI initial detection
After=lightdm.service

[Service]
Type=oneshot
ExecStart=/home/firefly/hdmi-switch.sh
RemainAfterExit=yes

[Install]
WantedBy=multi-user.target
EOF

sudo systemctl enable hdmi-init.service
```

---

## 测试步骤

1. 执行以上四步
2. `sudo reboot`
3. 开机后 SSH 进去，执行 `~/sunshine.sh status` 确认当前模式
4. 插上 HDMI 显示器，等待约 5 秒，确认显示器有画面
5. 拔掉 HDMI，等待约 5 秒，确认 Sunshine 自动启动（Moonlight 能连上）

---

## 已知风险

| 风险 | 说明 |
|------|------|
| sudo 免密 | hdmi-switch.sh 由 udev 以 root 运行，无此问题；sunshine.sh 手动运行需要 firefly 用户有 sudo 权限 |
| 开机未插 HDMI | hdmi-init 检测无 HDMI，自动进入 dummy+Sunshine 模式，正常 |
| 开机已插 HDMI | hdmi-init 检测到 HDMI，切换真实显示，Sunshine 不启动 |
| lightdm 重启踢会话 | 切换模式时图形会话会重建，运行中的 GUI 程序会丢失 |

---

## 已发现问题与修复

### 问题1：/tmp/sunshine.log 和 /tmp/sunshine.pid 权限拒绝

**现象**：
```
./sunshine.sh: line 13: /tmp/sunshine.pid: Permission denied
./sunshine.sh: line 12: /tmp/sunshine.log: Permission denied
```

**原因**：之前用 `sudo` 运行过脚本，导致 `/tmp/sunshine.log` 和 `/tmp/sunshine.pid` 被 root 所有。

**修复**：
```bash
sudo rm -f /tmp/sunshine.log /tmp/sunshine.pid
./sunshine.sh start
```

**注意**：sunshine.sh 不需要也不应该用 `sudo` 运行，直接 `./sunshine.sh start` 即可。

---

### 问题2：Sunshine 运行但 Moonlight 找不到设备

**现象**：`./sunshine.sh status` 显示 Sunshine 正在运行，但 Moonlight 端看不到板子。

**原因**：hdmi-switch.sh 由 udev 以 root 运行，导致 sunshine 以 root 身份启动，读取的是 `/root/.config/sunshine/` 而非 firefly 用户的配置，且 h264_rkmpp 编码器初始化失败，fallback 到软件编码。

**修复**：
- `hdmi-switch.sh` 里用 `su - firefly` 启动 sunshine，确保始终以 firefly 用户运行
- `sunshine.sh start` 开头加清理 `/tmp/sunshine.log` 和 `/tmp/sunshine.pid` 的逻辑，避免 root 遗留文件导致权限拒绝

**排查命令**：
```bash
# 确认 sunshine 是哪个用户在跑
ps aux | grep sunshine

# 查看启动日志
cat /tmp/sunshine.log
```
