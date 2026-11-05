# 本地命名管道协议 v1

默认地址 `\\.\pipe\QuadSyncAudio.v1`。UTF-8、无 BOM、每行一个 JSON 对象（NDJSON），以 `\n` 终止；请求和响应顺序一一对应。一个客户端应串行发送并等待响应。单条请求最多 64 KiB。

通用响应：

```json
{"id":1,"ok":true,"data":{}}
{"id":2,"ok":false,"error":"Value outside supported range"}
```

`id` 可省略，响应中为 `null`。错误不会断开有效的管道连接；过长请求和 I/O 断开会关闭当前连接，服务端继续等待新客户端。

## 命令

### devices

```json
{"id":1,"command":"devices"}
```

返回 `data.devices` 数组，每项有 `id`、`name`、`containerId`、`supportsFloat48`。设备 ID 是 Windows 端点字符串，应使用枚举结果，不根据显示名称构造。

### settings

完整提交四路设置；全部字段校验通过后更新原子目标值。通道顺序为前左、前右、后左、后右。

```json
{"command":"settings","master":0.5,"mute":false,"channels":[{"gainDb":0,"delayMs":0},{"gainDb":-3,"delayMs":1.2},{"gainDb":-6,"distanceM":3.43},{"gainDb":0,"delayMs":10}]}
```

`gainDb` 范围 `[-60,6]`，`master` 范围 `[0,1]`；延迟支持 `delayMs:[0,100]` 或 `distanceM:[0,34.3]`。同时给出时使用 `distanceM`。静音保留 Master 值。控制参数在音频块边界读取，增益在采样级平滑。

### start / stop

```json
{"command":"start","deviceA":"端点 A 的 ID","deviceB":"端点 B 的 ID","source":"tone"}
{"command":"start","deviceA":"端点 A 的 ID","deviceB":"端点 B 的 ID","source":"wav","path":"D:\\music\\quad.wav","loop":true}
{"command":"stop"}
```

`start` 先停止已有输出和音源，再准备新音源并打开设备；任何阶段失败会返回错误，不恢复旧流。`stop` 可重复调用。

### pcm

外部音源先使用 `source:"external"` 启动。WPF 和其他控制程序不能同时连接默认管道；需要外部 JSON PCM 时使用自定义控制客户端，或将 `quadsync_engine` 链接到自己的 C++ 应用。C++ 接口适合持续高吞吐输入。

```json
{"command":"start","deviceA":"A_ID","deviceB":"B_ID","source":"external"}
{"command":"pcm","samples":[0.1,0.2,0.3,0.4,0.2,0.3,0.4,0.5]}
```

数组按四通道交错排列；长度必须为 4 的倍数，最多 4096 帧，同时受 64 KiB 消息限制。推荐每次 256 帧。返回 `data.acceptedFrames`，未接收的数据应保留并重试。`status.writableFrames` 提供输入缓冲剩余空间。使用内部音源时拒绝 `pcm`，避免多个生产者同时写入。

### status

```json
{"command":"status"}
```

| 字段 | 含义 |
| --- | --- |
| running | 两个输出流是否在运行 |
| peaks | 四路输出限幅前峰值；每次读取清零，面板自己做下降保持 |
| lagFrames | 经平滑的 A − B 源播放位置；正值表示 B 落后 |
| lagMicroseconds | lagFrames × 1e6 / 48000 |
| bufferLagFrames | 同一时刻两个源排队深度之差，未经控制滤波 |
| correcting | 是否已超过 64 帧并激活补偿 |
| ratio | B 的每输出帧读取源步进，1 表示不重采样 |
| bufferA / bufferB | 各设备的独占缓冲帧数 |
| underrunFrames | 缺少输入而补零的源帧数，包含启动预填充 |
| clippedSamples | 输出超过满刻度而被限幅的样本数 |
| writableFrames | 输入环形缓冲可写帧数 |
| eof | 不循环 WAV 已读到文件结尾 |
| error | 最近一次音频线程错误，重新启动时清空 |

### shutdown

```json
{"command":"shutdown"}
```

停止音源、释放两台设备、回复成功后退出后台。
