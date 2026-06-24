# 话题:MqttThread 与 StreamThread 两种重连设计对比

记录格式约定:核心一句话 / 要点N / 注意事项 / 代码位置参考

## 核心一句话
StreamThread(RTMP) 和 MqttThread 面对"连不上/断开"这件事,走的是两种完全不同的设计:StreamThread 自己手写了完整的重连循环(BUG-008 修复的三层逻辑),MqttThread 把重连完全交给 paho 库的 `automatic_reconnect` 选项处理——这个差异背后是两个功能在系统里的优先级不同,但也带来一个值得讲清楚的边界:库自带的自动重连只覆盖"连上之后断开",不覆盖"启动时第一次连接就失败"。

## 要点1:两条线程对"重连"的责任划分不同
- `StreamThread`:自己实现 `reconnect_loop()`,无限重试、每3秒一次,重试期间持续排空队列防止反压(详见`stream_thread.md`要点4)。
- `MqttThread`:不写自己的重连逻辑,直接靠 `mqtt::connect_options::set_automatic_reconnect(true)`(line 27)让 paho 库内部处理。
- 这个差异是合理的优先级排序:RTMP推流是系统的交付物(用户最终要看的视频画面),做到"自动恢复"的工程投入值得;MQTT上报是辅助性的检测结果通知,用库自带的能力覆盖大部分场景已经够用,没必要重复造一套重连状态机。

## 要点2:`automatic_reconnect` 覆盖的范围——只管"连上之后断开"
paho 的自动重连是靠 `connection_lost` 回调触发的,前提是连接**曾经成功建立过**。
```cpp
try {
    client.connect(opts)->wait();          // 如果这一步直接失败……
} catch (const std::exception& e) {
    std::cerr << "MqttThread: connect failed: " << e.what() << "\n";
    return;                                 // ……线程直接退出,不会重试
}
```
- **场景A(已覆盖)**:启动时连上了,运行中broker重启或网络抖动断开→`automatic_reconnect` 接管,自动恢复,不需要任何额外代码。
- **场景B(没覆盖)**:启动时broker还没起来,`connect()->wait()` 同步抛异常→走到 `catch` 里直接 `return`,线程当场退出。`automatic_reconnect` 的重连逻辑从未被触发,因为它依赖"先前连接成功过"这个前提,这里从一开始就没成立。
- 一句话:`automatic_reconnect` 不是"管初始连接也会重试"的开关,这是容易被想当然的一点。

## 要点3:线程死亡后对管道的影响——跟 StreamThread 的反压问题不是一回事
`InferThread.cpp:317` 的 `mqtt_queue_.push(summary, 100)` 带100ms超时。MqttThread线程退出后没人pop这个队列,但push只会等最多100ms然后超时丢弃,不会像 `enc_queue`(容量1、阻塞push)那样级联反压到CaptureThread——所以场景B不会重演BUG-008那种"帧率跌到5fps"的连锁故障,影响被限制在"MQTT上报这一个功能"内,不影响视频/检测主链路。
- 代价是:场景B发生后,MQTT上报永久失效,只在启动时打一行 `connect failed` 日志,之后没有任何重复提示——是否需要额外的观测手段(比如周期性健康检查日志),取决于这条上报链路在实际部署里的重要程度。

## 注意事项
1. 中性讲两种设计的差异,不要把场景B定性为"bug"——MQTT上报本身是best-effort定位,这个设计选择是合理的,只是有一个容易被问出来的边界条件(场景B)。
2. 被追问"那场景B要怎么补"时,可以提思路(比如外层加一个轻量重试或定期健康检查),但不需要主动说"这里有个bug该修",除非面试官明确往这个方向问。

## 代码位置参考
`src/mqtt/MqttThread.cpp`
- run(): connect+自动重连配置 (line 19-27), 连接失败直接return (line 29-35), 主循环pop+publish (line 37-46)
`src/infer/InferThread.cpp`
- mqtt_queue push(带100ms超时) (line 317)
`src/stream/StreamThread.cpp`(对比对象,详见`stream_thread.md`)
- reconnect_loop: 手写无限重试 (line 194-212)
