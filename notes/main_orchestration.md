# 话题:main.cpp 整体编排

记录格式约定:核心一句话 / 要点N / 注意事项 / 代码位置参考

## 核心一句话
main.cpp 本身没有业务逻辑,纯粹是把五个线程类+五个队列/共享结构拼装起来,适合面试官问"画一下整体架构"或"线程是怎么组织起来的"时用;真正有讲点的是启动/关闭顺序为什么反着来,以及主循环为什么用轮询而不是条件变量。

## 要点1:五线程+五队列的拓扑全貌
```
CaptureThread ──┬──→ [enc_queue, 容量1, 阻塞]   → EncodeThread → [stream_queue, 容量2] → StreamThread
                |                                        ↓ 读取
                |                                SharedDetections
                |                                        ↑
                └──→ [infer_queue, 容量2, 非阻塞] → InferThread  ──→ [mqtt_queue, 容量16] → MqttThread
```
- 这张图把前面几个话题(BlockingQueue、Infer/Encode解耦、MqttThread)串成一条完整的总览,面试官问整体架构时直接画这个。
- 各队列容量都对应一段具体延迟预算:`enc_queue`=1≈68ms,`stream_queue`=2≈137ms,这些数字在`encode_thread.md`/`stream_thread.md`里有展开。

## 要点2:启动顺序——下游先于上游
```cpp
mqtter.start(); streamer.start(); encoder.start(); infer.start(); capture.start();
```
先启动消费者(MQTT/RTMP/编码),最后启动生产者(采集)。目的是避免"上游已经在产数据,下游还没准备好接"——虽然`BlockingQueue`本身push会阻塞等待不会丢数据,但确保整条链路都已经在`pop()`等待状态再开始喂第一帧,是更稳妥的初始化顺序。

## 要点3:关闭顺序——先停采集,再依次close队列让下游感知EOF
```cpp
capture.stop();
enc_queue.close(); infer_queue.close();
encoder.stop(); infer.stop();
stream_queue.close();
streamer.stop();
mqtt_queue.close(); mqtter.stop();
```
跟启动顺序正好相反:先停上游(不再产生新数据),再`close()`对应队列触发`closed_`标志,下游线程因为`blocking_queue.md`里"追问3"讲的机制(等待条件里带`|| closed_`)能自然感知到退出信号、跑完队列里剩余数据后退出,不需要强制kill线程。

## 要点4:主循环用 atomic<bool>+轮询,不用条件变量
```cpp
static std::atomic<bool> g_running{true};
static void on_signal(int) { g_running = false; }
...
while (g_running) std::this_thread::sleep_for(std::chrono::milliseconds(100));
```
对比`BlockingQueue`内部用条件变量做事件驱动等待,这里主线程退化成了100ms粒度的忙等。
- 追问:为什么这里不用条件变量等signal唤醒?——signal handler 是异步信号处理上下文,能做的事极其有限(异步信号安全函数集合很窄),不能在 handler 里直接 notify 条件变量做复杂操作;`atomic<bool>+轮询`是处理`SIGINT`/`SIGTERM`场景的常见简化写法。对"检测退出标志"这个场景,100ms轮询的CPU开销可以忽略,不需要为了"优雅"引入额外复杂度。

## 注意事项
1. 这部分是工程组织层面的考点,没有bug故事,面试官大概率只会问"画一下架构图"或"启动顺序有讲究吗",不会深挖到signal handling细节,准备好一两句结论即可。
2. 画图时记得带上`SharedDetections`这条跟队列拓扑并列的旁路连接,不要漏掉——它是InferThread和EncodeThread之间唯一的连接方式(详见`infer_encode_split.md`)。

## 代码位置参考
`src/main.cpp`
- 配置解析:CameraConfig/ModelConfig/DetectionConfig/EncodeConfig/StreamConfig/MqttConfig (line 36-78)
- 队列/共享结构声明 (line 88-97)
- 线程构造,按队列引用连接 (line 100-104)
- signal注册 (line 106-107)
- 主循环轮询 (line 121-123)
- 启动顺序 (line 110-119),关闭顺序 (line 127-139)
