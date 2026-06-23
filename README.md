# StackChan HTTP API

把 [StackChan](https://github.com/stack-chan/stack-chan) 变成一个通过 WiFi HTTP 接口控制的机器人。所有 AI/ML 计算在主机电脑上运行——ESP32 只负责硬件控制。

## 架构

```
主机电脑 (Mac/PC/树莓派)                StackChan (ESP32-S3)
┌───────────────────────┐               ┌────────────────────┐
│                       │               │                    │
│  你的 AI/应用/脚本      │─── HTTP ────▶ │  HTTP API 服务器    │
│                       │               │                    │
│  find_face.py         │◀── JPEG ────  │  摄像头 (GC0308)    │
│  (MediaPipe +         │─── 舵机 ───▶   │  舵机 (水平/俯仰)    │
│   InsightFace)        │─── 音频 ───▶   │  扬声器             │
│                       │◀── 音频 ────   │  麦克风             │
│  face_tracker.py      │─── 表情 ───▶   │  表情显示屏          │
│  speak.py             │               │  触摸传感器          │
│                       │               │                    │
└───────────────────────┘               └────────────────────┘
```

**为什么这样设计？** ESP32 跑不了 ML 模型，但硬件很好（摄像头、舵机、麦克风、扬声器、触摸）。通过 HTTP 暴露所有硬件能力，你可以在主机上用任何语言/框架/AI 来控制它。

## 硬件

- M5Stack StackChan 套件（CoreS3 ESP32-S3）
  - GC0308 摄像头、双麦克风、扬声器
  - 2个舵机（水平 ±128°、垂直 0-90°）
  - 触摸传感器（头顶3区电容触摸）
  - 2.0寸彩色显示屏（320x240）

## 快速开始

### 1. 烧录固件

```bash
# 安装 arduino-cli 和 M5Stack 板支持
brew install arduino-cli   # macOS
arduino-cli core install m5stack:esp32

# 配置 WiFi
cp firmware/config.h.example firmware/config.h
# 编辑 config.h，填入你的 WiFi 名称、密码和 IP 设置

# 编译烧录
arduino-cli compile --fqbn m5stack:esp32:m5stack_cores3 firmware/
arduino-cli upload --fqbn m5stack:esp32:m5stack_cores3 --port /dev/cu.usbmodem* firmware/
```

### 2. 验证

浏览器打开 `http://<StackChan的IP>/`，能看到控制面板就成功了。

### 3. 安装主机依赖

```bash
pip install opencv-python mediapipe numpy

# 可选：人脸身份识别
pip install insightface onnxruntime

# 可选：语音合成
pip install edge-tts        # 免费TTS
pip install requests        # ElevenLabs TTS 需要
```

### 4. 人脸查找

```bash
# 检测任意人脸
python host/find_face.py --host <StackChan的IP>

# 带身份识别（只找特定的人）
python host/register_face.py 照片1.jpg 照片2.jpg -o my_face.pkl
python host/find_face.py --host <StackChan的IP> --face-db my_face.pkl

# 持续人脸追踪
python host/face_tracker.py --host <StackChan的IP>
```

### 5. 语音合成

```bash
# 使用免费的 edge-tts
python host/speak.py --edge --host <StackChan的IP> "你好世界"

# 使用 ElevenLabs（需要 API key，在 .env 文件中配置）
python host/speak.py --host <StackChan的IP> "你好世界"
```

## HTTP API 接口

所有接口返回 JSON（`/camera` 返回 JPEG 图片）。

| 接口 | 方法 | 说明 |
|------|------|------|
| `/` | GET | 控制面板（HTML） |
| `/face?expr=<表情>` | GET | 设置表情：`neutral`、`happy`、`sad`、`angry`、`sleepy`、`doubt`、`love`、`eyeroll` |
| `/servo?yaw=N&pitch=N&speed=N` | GET | 控制舵机。Yaw: -1280 到 1280，Pitch: 0 到 900，Speed: 0 到 1000 |
| `/camera` | GET | 拍照，返回 JPEG（320x240） |
| `/status` | GET | 设备状态（电池、舵机位置、WiFi信号、运行时间） |
| `/home` | GET | 舵机回到初始位置 |
| `/touch` | GET | 触摸传感器读数（前、中、后） |
| `/record?seconds=N` | GET | 录音，返回 WAV。可选：`vad=1` 语音活动检测，`led=0` 关LED |
| `/stream?port=N` | GET | 把麦克风 PCM 流推到主机 TCP 端口（16kHz mono int16）。主机断开即停 |
| `/play?url=<url>` | GET | 从 URL 拉取并播放 WAV（最大 2MB ≈ 62秒），支持唇形同步 |
| `/speech?text=<文字>` | GET | 显示字幕。可选：`dur=<毫秒>` 配合语音自动翻页 |
| `/volume` | GET | 麦克风音量（RMS + 峰值） |

## 人脸检测原理

`find_face.py` 的工作流程：

1. **检查当前朝向** —— 拍两帧双重确认（防误检）
2. **没找到** —— 扫描 12 个预设舵机角度，逐个拍照检测
3. **身份验证**（可选） —— 用 InsightFace ArcFace 将检测到的人脸与预存的 embedding 做余弦相似度比对
4. **居中** —— 比例控制器微调舵机，把人脸放到画面中央
5. **输出** —— JSON 结果，包含置信度、位置、相似度

`face_tracker.py` 持续运行，两种模式：
- **Lazy**（默认）：每 3-10 秒看一眼，大幅移动才转头。安静不扰人。
- **Active**：每 0.3 秒检测一次，紧跟人脸。响应快但舵机声音大。

## 注意事项

- **GC0308 摄像头**：没有硬件 JPEG 编码器，用 RGB565 → 软件 JPEG 转换。画质低（0.3MP）但做人脸检测够用。
- **麦克风和扬声器共用 I2S 总线**：半双工，不能同时录音和播放。
- **macOS 用户**：Python `socket` 连局域网设备可能报 `Errno 65`。项目里的 `curl_session.py` 通过 curl 子进程绕过了这个问题。
- **WiFi 睡眠已关闭**：固件里禁用了 WiFi 省电模式，避免空闲后首次连接超时。
- **WAV 转换**：macOS 用 `afconvert`，Linux 需要安装 `ffmpeg`。

## License

MIT
