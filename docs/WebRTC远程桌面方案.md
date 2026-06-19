# RK3588S WebRTC 远程桌面方案

## 整体架构

```
板子桌面(X11虚拟显示)
        ↓
   ximagesrc 抓屏
        ↓
  videoconvert 格式转换
        ↓
  mpph264enc MPP硬件编码
        ↓
   rtph264pay RTP打包
        ↓
     webrtcbin
        ↓
   WebSocket 信令服务器
        ↓
   浏览器 WebRTC
        ↓
  鼠标键盘 DataChannel
        ↓
   xdotool 注入输入
```

---

## 组件说明

### 1. 虚拟显示（xserver-xorg-video-dummy）

板子没有接物理显示器，用虚拟显示驱动创建一个 1920x1080 的虚拟桌面（`:0`），
GStreamer 的 `ximagesrc` 从这里抓屏。

### 2. GStreamer Pipeline

```
ximagesrc display-name=:0 !
video/x-raw,framerate=30/1 !
videoconvert !
video/x-raw,format=NV12 !
mpph264enc !
rtph264pay config-interval=-1 pt=96 aggregate-mode=zero-latency !
application/x-rtp,media=video,encoding-name=H264,payload=96 !
webrtcbin name=webrtc bundle-policy=max-bundle latency=0
         stun-server=stun://stun.l.google.com:19302
```

| 元素 | 作用 |
|------|------|
| `ximagesrc` | 从 X11 虚拟显示抓屏，30fps |
| `videoconvert` | BGR → NV12 格式转换 |
| `mpph264enc` | RK3588S MPP VPU 硬件 H264 编码 |
| `rtph264pay` | H264 打包成 RTP，`aggregate-mode=zero-latency` 降低打包延迟 |
| `webrtcbin` | WebRTC 核心，处理 ICE/DTLS/SRTP，`latency=0` 降低缓冲 |

### 3. 信令服务器（server.py）

WebRTC 建连需要交换 SDP 和 ICE candidate，信令服务器用 WebSocket 承载：

```
板子 webrtcbin 生成 Offer SDP
        ↓ WebSocket
     浏览器收到 Offer
        ↓
   浏览器生成 Answer SDP
        ↓ WebSocket
    板子设置 Remote SDP
        ↓
   双方交换 ICE candidate
        ↓
      ICE 配对成功
        ↓
   P2P 视频流直连建立
```

**ICE candidate 时序问题**：webrtcbin 在 Answer 设置之前就可能触发
`on-ice-candidate` 回调，需要缓存 candidate，等 Answer set 之后再发送，
否则浏览器收到 candidate 时还没有 remote description，会丢弃。

### 4. ICE / mDNS 问题

Chrome 默认开启 mDNS 混淆，把本地 IP 替换成 `.local` 地址（如
`c69edc79-28b1-4075-8b99-b3d62xxx.local`），板子上的 libnice 无法解析
导致 ICE 配对失败。

**解决方法**：
```
chrome://flags/#enable-webrtc-hide-local-ips-with-mdns → Disabled
```

### 5. 输入控制（xdotool）

浏览器通过 WebSocket DataChannel 发送输入事件，板子上用 `xdotool` 注入：

| 事件 | xdotool 命令 |
|------|-------------|
| 鼠标移动 | `xdotool mousemove x y` |
| 鼠标按下 | `xdotool mousedown btn` |
| 鼠标释放 | `xdotool mouseup btn` |
| 键盘按下 | `xdotool keydown key` |
| 键盘释放 | `xdotool keyup key` |

浏览器的 `e.key` 和 xdotool 的按键名不同，需要映射：

| 浏览器 e.key | xdotool key |
|-------------|-------------|
| `Enter` | `Return` |
| `Backspace` | `BackSpace` |
| `ArrowUp` | `Up` |
| `ArrowDown` | `Down` |
| `ArrowLeft` | `Left` |
| `ArrowRight` | `Right` |
| ` `（空格）| `space` |

---

## 文件结构

```
~/webrtc-desktop/
├── server.py      # 信令服务器 + GStreamer pipeline
├── index.html     # 浏览器端 WebRTC + 输入控制
└── start.sh       # 启动脚本
```

## 启动方式

```bash
# 终端1：信令服务器
cd ~/webrtc-desktop && DISPLAY=:0 python3 server.py

# 终端2：HTTP 服务器（托管 index.html）
python3 -m http.server 8080 --directory ~/webrtc-desktop
```

浏览器访问：`http://192.168.1.200:8080`

---

## 依赖

| 依赖 | 版本 | 说明 |
|------|------|------|
| GStreamer | 1.20.3 | 已内置 |
| gstreamer-rockchip | 1.14.4 | MPP 编解码插件，已内置 |
| gstreamer1.0-nice | 0.1.18 | ICE 协议插件 |
| websockets | 16.0 | Python WebSocket 库 |
| PyGObject | 3.42.1 | GStreamer Python 绑定 |
| xdotool | - | 输入注入工具 |
| xserver-xorg-video-dummy | - | 虚拟显示驱动 |

---

## 已知问题

| 问题 | 原因 | 状态 |
|------|------|------|
| 延迟 1-2 秒 | webrtcbin jitter buffer + gop=30 | 待优化 |
| Chrome mDNS 需手动关闭 | Chrome 安全策略 | 需每台电脑配置一次 |
| 键盘需点击视频获焦 | 浏览器焦点机制 | 已改为 document 监听 |
| 视频拖拽晃动 | 浏览器原生媒体拖拽行为 | 已加 draggable=false 修复 |

---

## 与 Sunshine+Moonlight 对比

| | Sunshine+Moonlight | GStreamer+WebRTC |
|---|---|---|
| 延迟 | 极低（游戏级） | 1-2 秒（待优化） |
| MPP 硬件编码 | 待解决 | ✅ 直接用 |
| 客户端 | 需装 Moonlight | 浏览器即可 |
| 鼠标键盘控制 | 原生透传 | xdotool 注入 |
| 搭建难度 | 中（已完成） | 中（已完成） |
