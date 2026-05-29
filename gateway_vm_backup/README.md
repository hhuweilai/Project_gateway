## 项目概述

以 **i.MX6ULL (ARM Cortex-A7)** 为核心，搭载 **OV5640 MIPI CSI 摄像头**，实现工业缺陷检测边缘网关。视频帧通过 TCP 实时传输至 PC 端运行 YOLO 推理，检测结果回传开发板触发继电器/蜂鸣器/RGB LED 报警。

```
OV5640 → V4L2 mmap → RingBuffer → TLV 分片 → TCP → PC (YOLO)
                                                    │
[继电器+蜂鸣器+LED] ← trigger_alarm() ← AI_RESULT ←──┘
```

## 硬件架构

| 组件 | 型号 | 接口 |
|------|------|------|
| 主控 | i.MX6ULL (Cortex-A7 @ 792MHz) | — |
| 摄像头 | OV5640 | MIPI CSI (2-lane) |
| 光照传感器 | BH1750 | I2C1 (0x23) |
| 继电器 | GPIO2_IO00 | GPIO32 |
| 蜂鸣器 | GPIO2_IO01 | GPIO33 |
| RGB LED | GPIO2_IO02/03/04 | GPIO34/35/36 |
| 红外传感器 | GPIO2_IO05 | GPIO37 |

## 软件架构

```
┌─────────────────────────────────────────────┐
│                  gateway_app                 │
│                                              │
│  ┌──────────┐  ┌──────────┐  ┌───────────┐  │
│  │ Capture   │  │ Network   │  │  Main      │  │
│  │ Thread    │  │ Thread    │  │  Thread    │  │
│  │           │  │           │  │            │  │
│  │ V4L2 mmap │  │ RingBuf   │  │ epoll      │  │
│  │    ↓      │  │  .pop()   │  │  ├ stdin   │  │
│  │ RingBuf   │  │    ↓      │  │  ├ MES TCP │  │
│  │ .push()   │  │ TLV send  │  │  └ 心跳    │  │
│  └──────────┘  └──────────┘  └───────────┘  │
└─────────────────────────────────────────────┘
         │                                    │
         ▼                                    ▼
   /dev/video0                         PC: ai_receiver.py
                                       (YOLO 推理)
```

## 目录结构

```
gateway/
├── CMakeLists.txt              # 构建配置（5 个阶段目标）
├── cmake/
│   └── arm-linux-gnueabihf.cmake  # ARM 交叉编译工具链
├── src/
│   ├── gateway_app.cpp         # ★ 网关主程序（3 线程 + epoll）
│   ├── ai_receiver.py          # ★ PC 端 AI 推理接收器
│   ├── video_capture.c/.h      # V4L2 mmap 视频采集
│   ├── frame_sender.c/.h       # 大帧 TLV 分片发送
│   ├── tlv_protocol.c/.h       # TLV 协议编解码 + 流式解析器
│   ├── mes_handler.c/.h        # MES 事件（JSON over TLV）
│   ├── ring_buffer.h           # C++11 线程安全环形缓冲区
│   ├── gpio_ops.c/.h           # GPIO 操作（sysfs）
│   ├── tcp_server.c            # MES 模拟服务器（VM 端）
│   ├── tcp_client.c            # MES 客户端（epoll + 自动重连）
│   ├── frame_receiver.c        # 视频帧接收 + 分片重组
│   ├── camera_test.c           # V4L2 单帧抓取测试
│   ├── peripheral_ctrl.c       # 外设独立控制程序
│   ├── bh1750_test.c           # BH1750 用户态测试
│   ├── gpio_led.c              # 系统 LED 闪烁测试
│   └── main.c                  # 交叉编译 Hello World
├── driver/
│   ├── hello_mod.c             # 最小内核模块
│   ├── virtual_bh1750.c        # 虚拟 BH1750 I2C 字符驱动
│   └── Makefile                # Kbuild
├── models/
│   └── yolo11n.pt              # YOLO11n 权重（替换为自定义模型）
├── build/                      # x86_64 本地构建
└── build_arm/                  # ARM 交叉编译构建
```

## TLV 协议

自定义应用层协议，帧格式（大端序）：

```
[Tag:1B][Length:2B][Value:Length B][CRC:1B]
```

| Tag | 名称 | 方向 | 说明 |
|-----|------|------|------|
| 0x01 | HEARTBEAT_REQ | GW→PC | 心跳请求 |
| 0x02 | HEARTBEAT_RSP | PC→GW | 心跳响应 |
| 0x10 | MES_EVENT | GW→PC | 启动/缺陷事件 (JSON) |
| 0x11 | MES_ACK | PC→GW | 确认 |
| 0x20 | FRAME_DATA | GW→PC | 视频帧分片 (≤32KB) |
| 0x22 | FRAME_START | GW→PC | 帧起始 (4B 总长) |
| 0x30 | AI_RESULT | PC→GW | AI 检测结果 (JSON) |
| 0x31 | AI_READY | PC→GW | AI 就绪通知 |
| 0x32 | AI_CMD | GW→PC | AI 控制指令 |

CRC 算法：`tag ^ len_hi ^ len_lo ^ value[0] ^ ... ^ value[n-1]`

## 快速开始

### 环境要求

| 组件 | 版本/说明 |
|------|----------|
| 交叉编译器 | Linaro GCC 5.3.1 (arm-linux-gnueabihf) |
| CMake | ≥ 3.10 |
| Python | ≥ 3.8 |
| Python 包 | `opencv-python numpy ultralytics` |

### 编译（ARM）

```bash
cd build_arm
cmake .. -DCMAKE_TOOLCHAIN_FILE=../cmake/arm-linux-gnueabihf.cmake
make gateway_app -j$(nproc)
```

### 编译（x86_64 本地测试）

```bash
cd build
cmake ..
make tcp_server frame_receiver -j$(nproc)
```

### 部署 & 运行

```bash
# 1. 部署网关到开发板
scp -o HostKeyAlgorithms=+ssh-rsa build_arm/gateway_app root@192.168.5.20:/tmp/

# 2. VM 端启动 AI 接收器
fuser -k 8899/tcp
python3 src/ai_receiver.py 8899 models/yolo11n.pt

# 3. 开发板启动网关
cd /tmp && ./gateway_app 192.168.5.30 8899
```

### 交互命令

| 按键 | 功能 |
|------|------|
| `d` | 手动发送缺陷事件 |
| `s` | 显示运行状态 |
| `h` | 显示帮助 |
| `q` | 退出 |

## 摄像头驱动加载（开发板）

```bash
modprobe v4l2-int-device
modprobe mxc_mipi_csi
modprobe mx6s_capture
modprobe ov5640_camera_mipi
modprobe ipu_prp_enc ipu_csi_enc ipu_still ipu_bg_overlay_sdc ipu_fg_overlay_sdc
modprobe mxc_v4l2_capture
```

## 关键技术点

- **V4L2 mmap 零拷贝采集**：4 缓冲区 DQBUF/QBUF 循环，避免 CPU 拷贝
- **C++11 多线程**：采集/网络/主控三线程，环形缓冲区解耦
- **epoll EPOLLET**：边沿触发 + 非阻塞 I/O，高效事件驱动
- **自定义 TLV 协议**：流式解析状态机 + XOR CRC 校验
- **大帧分片传输**：614KB YUYV 帧 → FRAME_START + 多个 32KB FRAME_DATA
- **AI 推理与 GPIO 联动**：YOLO 检测 → TLV_AI_RESULT → 继电器/蜂鸣器/LED 报警
- **Linux 内核驱动**：I2C 字符设备驱动（BH1750）、Kbuild 编译
- **ARM 交叉编译**：CMake 工具链文件 + Linaro GCC

## 阶段划分

| 阶段 | 内容 | 关键产出 |
|------|------|---------|
| 1 | 交叉编译环境 + GPIO + 摄像头基础 | `main.c`, `gpio_led.c`, `camera_test.c` |
| 2 | BH1750 I2C 驱动 + 外设控制 | `virtual_bh1750.c`, `peripheral_ctrl.c` |
| 3 | TLV 协议 + epoll 通信引擎 | `tlv_protocol.c`, `tcp_server.c`, `tcp_client.c` |
| 4 | V4L2 mmap 采集 + 多线程 + 大帧分片 | `video_capture.c`, `frame_sender.c`, `gateway_app.cpp` |
| 5 | AI 推理集成 + 全链路闭环 | `ai_receiver.py`, `mes_handler.c` AI_RESULT |

## License

MIT
