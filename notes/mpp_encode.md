# 话题:MPP 编码常见问题

记录格式约定:核心一句话 / 追问N / 注意事项 / 代码位置参考

## 核心一句话
面试官一般顺着"概念→buffer→参数→流程→格式"五块问,不会深究到代码行级别,每块给2-3句话的具体回答就够,追问再展开。

## 要点1:MPP 是什么
Rockchip 的硬件多媒体处理框架,统一了编解码/缩放等硬件IP的调用接口。本项目里只用了编码这一块(`MPP_CTX_ENC`),把摄像头采集的画面编成 H.264。

## 要点2:Buffer 怎么管理的
通过 `MppBufferGroup` 申请类型为 `MPP_BUFFER_TYPE_DRM` 的内存,这种内存是硬件编码器能直接 DMA 访问的物理内存,跟 CPU `malloc` 的堆内存是两种东西——传普通堆内存硬件读不到。每帧编码前先拿到这块 DRM buffer 的指针，直接作为 RGA 转换目标地址写入，从而实现零拷贝（Zero-Copy），省去了多余的 `memcpy` 开销（如 RGA 失败则 fallback 回 CPU 软转并 memcpy）。

## 要点3:参数怎么配的
用一个配置对象 `MppEncCfg` 把所有参数(分辨率、格式、码率模式、GOP)一次性塞进去,再调一次 `control(MPP_ENC_SET_CFG, ...)` 提交给硬件,不是逐个 setter 调用。码率控制选 CBR(恒定码率),因为是给 RTMP 直播推流用,带宽要稳定,不像录像场景能用 VBR 换更好画质。
- 可能追问:GOP 设置 of 权衡——GOP 小(关键帧密)抗丢包、方便随机访问但码率开销大;GOP 大反过来。本项目 GOP=8@14.6fps≈550ms一个关键帧,是基于低帧率+实时性优先做的选择。

## 要点4:编码主循环逻辑
`encode_put_frame` 喂一帧,`encode_get_packet` 取结果,这两个是异步关系——硬件内部有流水线缓冲,一次 put 不一定对应一次 get,可能攒了多个包,所以要用 while 循环把当前能取的包全部取干净,否则会有包积压、时间戳错位问题。

## 要点5:为什么要转 NV12
硬件编码器只吃固定格式,这个项目配的是 NV12(YUV420SP),摄像头出来的是 YUYV(4:2:2打包格式),格式不匹配,软件层要先转换一次再喂给编码器。
- 可能追问:转换是 CPU 软转还是硬件(RGA)——本项目采用 RGA 硬件加速优先的策略，直接在 MPP DRM Buffer 上完成 YUYV 到 NV12 转换与画框。当 RGA 失败时，回退到 CPU 软转（`yuyv_to_nv12`）并拷贝兜底。

## 注意事项
1. 不要把五块都展开成长篇大论,每块先给一两句结论,等面试官追问再深入到 DRM buffer / GOP 权衡这两个最容易被深挖的点。
2. 讲清楚如何通过 RGA 直接写入 DRM Buffer 消除 memcpy 这一优化点（Zero-Copy 设计），这是 RK3588 多媒体开发中极具说服力的实践经验。

## 代码位置参考
`src/encode/EncodeThread.cpp`
- MPP_CTX_ENC 初始化 + MppEncCfg 配置
- MPP_BUFFER_TYPE_DRM buffer group 申请与管理
- rga_yuyv_to_nv12 硬件加速与 yuyv_to_nv12 软转换兜底
- run() 主循环中的 DRM Buffer 获取、RGA 零拷贝转换与画框及硬编码流程
