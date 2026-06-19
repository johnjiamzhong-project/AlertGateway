# 第五阶段：MPP硬解（2周）

> 目标：在RK3588S上用MPP硬件解码，FFmpeg对接rkmpp，RambosPlayer移植思路梳理

---

## 一、MPP是什么

MPP（Media Process Platform）是Rockchip提供的硬件编解码框架，直接调用SoC内置的VPU（Video Processing Unit），绕过CPU软解。

RK3588S的VPU能力：
- H.265 解码：8K@60fps
- H.264 解码：4K@60fps
- AV1 解码：4K@60fps
- H.264/H.265 编码：4K@60fps
- CPU占用：硬解时接近0%

### MPP在软件栈中的位置

```
你的应用程序
    ↓
FFmpeg（demux、封装、音频）
    ↓
MPP（硬件解码，替换软解码器）
    ↓
VPU 硬件
    ↓
DRM/显示
```

FFmpeg不需要完全替换，只把解码器换成rkmpp即可，其他（demux、音频、网络协议）还是FFmpeg负责。

---

## 二、核心概念

### 2.1 三个关键结构

**MppCtx** — MPP实例句柄，所有操作的入口

**MppApi** — 函数接口表，通过它调用所有MPP函数
```c
MppCtx ctx;
MppApi *mpi;
mpp_create(&ctx, &mpi);
```

**MppPacket / MppFrame**
- `MppPacket`：输入，压缩的码流数据（H.264/H.265 NAL单元）
- `MppFrame`：输出，解码后的图像帧（YUV格式）

### 2.2 工作流程

```
读取码流数据
    ↓
封装成 MppPacket
    ↓
mpi->decode_put_packet(ctx, packet)  ← 送入MPP
    ↓
mpi->decode_get_frame(ctx, &frame)   ← 取出解码帧
    ↓
MppFrame → 渲染/处理
```

### 2.3 缓冲区管理

MPP使用DMA buffer（`MppBuffer`），物理连续内存，VPU可以直接访问：

```c
MppBufferGroup group;
mpp_buffer_group_get_internal(&group, MPP_BUFFER_TYPE_DRM);
// 解码输出帧的buffer从这个group里分配
```

帧数据在DMA buffer里，零拷贝传给显示或后处理。

---

## 三、环境搭建

### 3.1 确认MPP库是否已安装

```bash
# 板子上
dpkg -l | grep -i mpp
ls /usr/lib/aarch64-linux-gnu/ | grep mpp
ls /usr/include/rockchip/
```

**Firefly ROC-RK3588S-PC实际结果（已验证）：**

```
ii  librockchip-mpp-dev    1.5.0-1    arm64    Media Process Platform
ii  librockchip-mpp1       1.5.0-1    arm64    Media Process Platform
ii  libv4l-rkmpp           1.7.0-1    arm64    A rockchip-mpp V4L2 wrapper plugin
ii  rockchip-mpp-demos     1.5.0-1    arm64    Media Process Platform Demos
```

头文件位置：`/usr/include/rockchip/`（mpp_buffer.h、mpp_frame.h、mpp_packet.h等）

官方demo位置：`/usr/bin/mpi_dec_test`

### 3.2 如果没有，安装MPP

```bash
sudo apt update
sudo apt install librockchip-mpp-dev librockchip-mpp1
```

Firefly的Ubuntu镜像预装了MPP，一般不需要手动安装。

### 3.3 确认VPU设备节点

```bash
ls /dev/mpp*    # 应该有 /dev/mpp_service
ls /dev/dri/    # DRM设备
```

### 3.4 确认FFmpeg带rkmpp支持

```bash
ffmpeg -codecs 2>/dev/null | grep rkmpp
ffmpeg -hwaccels
```

**注意：Firefly Ubuntu 22.04自带FFmpeg 4.4.2是官方apt包，编译时未开启rkmpp。**

```
Hardware acceleration methods:
vdpau / cuda / vaapi / drm / opencl   ← 没有rkmpp
```

需要重新编译FFmpeg，见第六节。

---

## 四、第一个MPP程序：mpi_dec_test（已验证）

Firefly Ubuntu镜像预装了官方demo，直接可用。

### 4.1 确认demo位置

```bash
find / -name "mpi_dec_test" 2>/dev/null
# 输出：/usr/bin/mpi_dec_test
```

### 4.2 准备测试文件

MPP直接解码需要Annex B格式裸流，从mp4提取：

```bash
ffmpeg -i input.mp4 -vcodec copy -an -bsf:v h264_mp4toannexb test.h264
```

### 4.3 跑测试

```bash
mpi_dec_test -t 7 -i test.h264   # -t 7 是H.264
```

**Firefly RK3588S实际输出（已验证）：**

```
decode 800 frames time 4149 ms delay 37 ms fps 192.79
test success max memory 5.98 MB
```

**192fps**，CPU占用接近0%，VPU全程负责解码。

✅ 验收达成：mpi_dec_test跑通，硬解fps 192.79

---

## 五、最小MPP解码Demo（C代码）

自己写一个最小可跑的MPP解码程序，理解完整流程。

### 5.1 CMakeLists.txt

```cmake
cmake_minimum_required(VERSION 3.10)
project(mpp_demo)

find_library(MPP_LIB rockchip_mpp)
include_directories(/usr/include/rockchip)

add_executable(mpp_demo main.c)
target_link_libraries(mpp_demo ${MPP_LIB})
```

### 5.2 main.c 核心结构

```c
#include <rockchip/rk_mpi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define READ_BUF_SIZE (1024 * 1024)  // 1MB读取缓冲

int main(int argc, char *argv[])
{
    if (argc < 2) {
        printf("Usage: %s input.h264\n", argv[0]);
        return -1;
    }

    // 1. 创建MPP实例
    MppCtx ctx = NULL;
    MppApi *mpi = NULL;
    MPP_RET ret = mpp_create(&ctx, &mpi);
    if (ret != MPP_OK) {
        printf("mpp_create failed: %d\n", ret);
        return -1;
    }

    // 2. 初始化，指定解码类型
    ret = mpp_init(ctx, MPP_CTX_DEC, MPP_VIDEO_CodingAVC);  // H.264
    if (ret != MPP_OK) {
        printf("mpp_init failed: %d\n", ret);
        mpp_destroy(ctx);
        return -1;
    }

    // 3. 配置解码参数（可选，使用默认值）
    MppDecCfg cfg = NULL;
    mpp_dec_cfg_init(&cfg);
    mpi->control(ctx, MPP_DEC_SET_CFG, cfg);
    mpp_dec_cfg_deinit(cfg);

    // 4. 打开输入文件
    FILE *fp = fopen(argv[1], "rb");
    if (!fp) {
        printf("open file failed\n");
        mpp_destroy(ctx);
        return -1;
    }

    char *buf = malloc(READ_BUF_SIZE);
    int frame_count = 0;

    // 5. 解码循环
    while (1) {
        // 读取数据
        size_t read_size = fread(buf, 1, READ_BUF_SIZE, fp);
        int eos = (read_size == 0);

        // 封装MppPacket
        MppPacket packet = NULL;
        mpp_packet_init(&packet, buf, read_size);
        if (eos)
            mpp_packet_set_eos(packet);

        // 送入MPP
        ret = mpi->decode_put_packet(ctx, packet);
        mpp_packet_deinit(&packet);

        // 取出解码帧
        MppFrame frame = NULL;
        ret = mpi->decode_get_frame(ctx, &frame);
        if (ret == MPP_OK && frame) {
            if (mpp_frame_get_info_change(frame)) {
                // 分辨率变化，需要重新分配buffer
                MppBufferGroup group = NULL;
                mpp_buffer_group_get_internal(&group, MPP_BUFFER_TYPE_DRM);
                mpi->control(ctx, MPP_DEC_SET_EXT_BUF_GROUP, group);
                mpi->control(ctx, MPP_DEC_SET_INFO_CHANGE_READY, NULL);
            } else {
                frame_count++;
                int width  = mpp_frame_get_width(frame);
                int height = mpp_frame_get_height(frame);
                printf("frame %d: %dx%d\n", frame_count, width, height);
            }
            mpp_frame_deinit(&frame);
        }

        if (eos) break;
    }

    printf("total frames: %d\n", frame_count);

    // 6. 清理
    free(buf);
    fclose(fp);
    mpp_destroy(ctx);
    return 0;
}
```

### 5.3 编译运行

```bash
mkdir build && cd build
cmake ..
make -j4
./mpp_demo ../test.h264
```

看到帧数和分辨率输出，说明MPP硬解跑通了。

---

## 六、FFmpeg对接rkmpp

### 6.1 确认FFmpeg是否带rkmpp

```bash
ffmpeg -decoders 2>/dev/null | grep rkmpp
```

应该看到：
```
V..... h264_rkmpp          H.264 / AVC (Rockchip MPP)
V..... hevc_rkmpp          HEVC (Rockchip MPP)
```

### 6.2 重新编译FFmpeg（板子本地编译，推荐）

**装到非系统路径，不覆盖系统FFmpeg：**

```bash
# 装依赖
sudo apt install libdrm-dev librockchip-mpp-dev

# 下载FFmpeg源码
git clone --depth=1 https://github.com/FFmpeg/FFmpeg.git
cd FFmpeg

# 配置，开启rkmpp，装到独立目录
./configure \
    --enable-rkmpp \
    --enable-libdrm \
    --enable-shared \
    --disable-static \
    --prefix=/home/firefly/ffmpeg_rkmpp

make -j4
make install
```

编译时间约30~60分钟（RK3588S `make -j4`）。

使用时指定路径：
```bash
/home/firefly/ffmpeg_rkmpp/bin/ffmpeg -c:v h264_rkmpp -i input.mp4 -f null -
```

**如果后续需要WSL2交叉编译项目使用这套FFmpeg：**

```bash
# WSL2上把板子的FFmpeg库rsync过来作为sysroot
rsync -avz firefly@192.168.1.200:/home/firefly/ffmpeg_rkmpp/ ~/sysroot/ffmpeg_rkmpp/
rsync -avz firefly@192.168.1.200:/usr/include/rockchip/ ~/sysroot/usr/include/rockchip/
rsync -avz firefly@192.168.1.200:/usr/lib/aarch64-linux-gnu/librockchip_mpp* ~/sysroot/usr/lib/aarch64-linux-gnu/
```

### 6.3 ffmpeg命令行测试rkmpp

注意：编译的是shared库，运行前需要设置库路径：

```bash
export LD_LIBRARY_PATH=/home/firefly/ffmpeg_rkmpp/lib:$LD_LIBRARY_PATH
# 写入~/.bashrc永久生效
echo 'export LD_LIBRARY_PATH=/home/firefly/ffmpeg_rkmpp/lib:$LD_LIBRARY_PATH' >> ~/.bashrc
```

验证rkmpp解码器：

```bash
/home/firefly/ffmpeg_rkmpp/bin/ffmpeg -decoders 2>/dev/null | grep rkmpp
# 输出：
# V..... h264_rkmpp    h264 (rkmpp) (codec h264)
# V..... hevc_rkmpp    hevc (rkmpp) (codec hevc)
# V..... vp8_rkmpp     vp8 (rkmpp) (codec vp8)
# V..... vp9_rkmpp     vp9 (rkmpp) (codec vp9)
```

性能测试命令：

```bash
# 硬解
/home/firefly/ffmpeg_rkmpp/bin/ffmpeg -c:v h264_rkmpp -i input.mp4 -f null - 2>&1 | tail -3

# 软解对比
ffmpeg -i input.mp4 -f null - 2>&1 | tail -3
```

**实际测试结果（1920x814 H.264 High Profile 24fps 11Mbps，800帧）：**

| | 软解（FFmpeg 4.4.2） | rkmpp硬解 |
|---|---|---|
| 耗时 | ~2.3s | 0.58s |
| 速度 | 14.5x | **56.8x** |
| CPU | 跑满1~2核 | 接近0% |

硬解快了约**4倍**，CPU基本不参与。

### 6.4 代码中使用rkmpp

```c
// 用FFmpeg API指定rkmpp解码器
AVCodec *codec = avcodec_find_decoder_by_name("h264_rkmpp");
AVCodecContext *codec_ctx = avcodec_alloc_context3(codec);

// 关键：设置硬件加速
AVBufferRef *hw_device_ctx = NULL;
av_hwdevice_ctx_create(&hw_device_ctx, AV_HWDEVICE_TYPE_DRM, NULL, NULL, 0);
codec_ctx->hw_device_ctx = av_buffer_ref(hw_device_ctx);

avcodec_open2(codec_ctx, codec, NULL);
```

---

## 七、RambosPlayer移植思路

RambosPlayer已有D3D11VA硬解（Windows），移植到RK3588S替换为rkmpp。

### 7.1 需要替换的部分

| 模块 | Windows | RK3588S |
|---|---|---|
| 硬解API | D3D11VA | rkmpp |
| 渲染 | Direct3D 11 | OpenGL ES / DRM |
| 零拷贝 | D3D texture | DMA buffer |

### 7.2 移植策略

**最小改动方案**：用条件编译，不破坏Windows版本

```cpp
#ifdef __aarch64__
    // RK3588S路径
    codec = avcodec_find_decoder_by_name("h264_rkmpp");
    // DMA buffer → OpenGL ES纹理
#else
    // Windows路径
    codec = avcodec_find_decoder_by_name("h264_cuvid");
    // D3D11VA
#endif
```

**渲染路径**：
- DMA buffer（`MppBuffer`）→ `EGL_EXT_image_dma_buf_import` → OpenGL ES纹理
- 零拷贝，帧数据不经过CPU

### 7.3 移植步骤

1. 先让RambosPlayer在板子上用软解跑起来（验证Qt + FFmpeg基础流程）
2. 替换解码器为`h264_rkmpp`
3. 渲染路径从CPU memcpy改为DMA buffer直传
4. 测试RTMP/HTTP-FLV流媒体硬解性能

### 7.4 预期性能

| 场景 | 软解 | rkmpp硬解 |
|---|---|---|
| 1080P H.264 | CPU 60~80% | CPU <5% |
| 4K H.264 | 可能卡顿 | 流畅 |
| RTMP流 | 延迟高 | 低延迟 |

---

## 八、两周计划

### 第一周：跑通硬解

| 天 | 任务 |
|---|---|
| 1~2 | 确认MPP环境，mpp_decode_test跑通 |
| 3~4 | 自写最小Demo，理解MppPacket/MppFrame流程 |
| 5~7 | FFmpeg rkmpp编译/验证，命令行测试性能对比 |

### 第二周：FFmpeg集成+移植

| 天 | 任务 |
|---|---|
| 1~3 | FFmpeg代码层对接rkmpp，写解码测试程序 |
| 4~5 | RambosPlayer软解在板子上跑通 |
| 6~7 | 替换为rkmpp，测试性能，记录问题 |

---

## 九、验收标准

- [x] `mpi_dec_test` 跑通，输出帧率（**192.79fps，800帧，4149ms**）
- [ ] 自写Demo解码H.264文件，输出帧数
- [x] FFmpeg重新编译，`h264_rkmpp` 解码器可用（**h264/hevc/vp8/vp9全部支持**）
- [x] 硬解vs软解CPU占用对比数据（**硬解56.8x vs 软解14.5x，快4倍**）
- [ ] RambosPlayer在板子上用rkmpp播放1080P视频

---

## 十、常见问题

**Q: mpp_create返回失败**
检查`/dev/mpp_service`是否存在，权限是否正确：
```bash
ls -la /dev/mpp_service
sudo chmod 666 /dev/mpp_service
# 或加入video组
sudo usermod -aG video $USER
```

**Q: decode_get_frame一直返回空**
MPP解码有延迟，需要送入足够数据才开始输出帧，正常现象，循环多送几包数据。

**Q: 帧数据格式**
MPP输出NV12（YUV420 semi-planar），不是RGB，显示前需要转换或用OpenGL ES shader处理。

**Q: FFmpeg编译太慢**
用交叉编译（WSL2），产物scp到板子，参考第四阶段的交叉编译工作流。

---

*文档版本：Phase 5 进行中*
*板子：Firefly ROC-RK3588S-PC 8GB*
*系统：Ubuntu 22.04 aarch64*
*MPP版本：1.5.0*
*FFmpeg系统版本：4.4.2（未含rkmpp，需重新编译）*
