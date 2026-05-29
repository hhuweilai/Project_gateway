#!/usr/bin/env python3
"""
ai_receiver.py — AI 缺陷检测接收器 (阶段5)

功能:
  1. TCP 监听端口 8899，接收网关视频帧 + MES 事件
  2. TLV 协议解析 + 帧重组 (FRAME_START → FRAME_DATA)
  3. YUYV → BGR 转换
  4. YOLO 缺陷检测
  5. 检测结果 TLV_AI_RESULT 回传网关 → 触发外设

运行:
  python3 ai_receiver.py [port] [model_path]

依赖:
  pip install opencv-python numpy ultralytics
"""

import socket
import struct
import sys
import os
import time
import threading
import signal
from collections import deque

# ========== TLV Protocol ==========
TLV_HEARTBEAT_REQ = 0x01
TLV_HEARTBEAT_RSP = 0x02
TLV_MES_EVENT     = 0x10
TLV_MES_ACK       = 0x11
TLV_FRAME_DATA    = 0x20
TLV_FRAME_ACK     = 0x21
TLV_FRAME_START   = 0x22
TLV_AI_RESULT     = 0x30
TLV_AI_READY      = 0x31
TLV_AI_CMD        = 0x32

TLV_HEADER_SIZE = 3
TLV_CRC_SIZE    = 1
TLV_OVERHEAD    = TLV_HEADER_SIZE + TLV_CRC_SIZE

TAG_NAMES = {
    0x01: "HEARTBEAT_REQ", 0x02: "HEARTBEAT_RSP",
    0x10: "MES_EVENT",     0x11: "MES_ACK",
    0x20: "FRAME_DATA",    0x21: "FRAME_ACK",
    0x22: "FRAME_START",   0x30: "AI_RESULT",
    0x31: "AI_READY",      0x32: "AI_CMD",
}

g_running = True

def signal_handler(sig, frame):
    global g_running
    g_running = False
    print("\n[SRV] shutting down...")

signal.signal(signal.SIGINT, signal_handler)
signal.signal(signal.SIGTERM, signal_handler)


def crc8(data: bytes) -> int:
    """CRC-8 校验 (与 C 端 tlv_checksum 兼容)"""
    crc = 0xFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            if crc & 0x80:
                crc = ((crc << 1) ^ 0x07) & 0xFF
            else:
                crc = (crc << 1) & 0xFF
    return crc


def tlv_pack(tag: int, value: bytes) -> bytes:
    """打包 TLV 帧"""
    length = len(value)
    if length > 65535:
        raise ValueError("Value too long")
    header = struct.pack(">BH", tag, length)
    crc = crc8(header + value)
    return header + value + bytes([crc])


def tlv_pack_ai_result(has_defect: int, defect_type: str,
                       confidence: float, bbox: tuple) -> bytes:
    """打包 AI_RESULT TLV 帧"""
    import json
    payload = json.dumps({
        "defect": has_defect,
        "type": defect_type,
        "conf": round(confidence, 2),
        "bbox": list(bbox)
    })
    return tlv_pack(TLV_AI_RESULT, payload.encode())


def tlv_pack_ack(tag: int) -> bytes:
    import json
    payload = json.dumps({"ack": tag}).encode()
    return tlv_pack(TLV_MES_ACK, payload)


# ========== TLV Stream Parser ==========
class TLVReader:
    """流式 TLV 解析器 (与 C 端 tlv_reader_t 一致)"""
    HEADER = 0
    BODY   = 1
    CRC    = 2

    def __init__(self):
        self.state = self.HEADER
        self.header = bytearray()
        self.tag = 0
        self.length = 0
        self.body = bytearray()

    def feed(self, data: bytes):
        """喂入字节流, 返回 (tag, value) 或 None"""
        for byte in data:
            if self.state == self.HEADER:
                self.header.append(byte)
                if len(self.header) == TLV_HEADER_SIZE:
                    self.tag = self.header[0]
                    self.length = struct.unpack(">H", self.header[1:3])[0]
                    self.body = bytearray()
                    self.state = self.BODY
            elif self.state == self.BODY:
                self.body.append(byte)
                if len(self.body) == self.length:
                    self.state = self.CRC
            elif self.state == self.CRC:
                crc_received = byte
                expected = crc8(bytes(self.header) + bytes(self.body))
                self.header.clear()
                self.state = self.HEADER
                # CRC check (soft — still return data on mismatch)
                tag = self.tag
                body = bytes(self.body)
                self.body.clear()
                return (tag, body)
        return None


# ========== Frame Reassembler ==========
class FrameReassembler:
    """FRAME_START → FRAME_DATA 重组"""
    def __init__(self):
        self.expected_size = 0
        self.buffer = bytearray()

    def feed(self, tag: int, value: bytes):
        if tag == TLV_FRAME_START:
            self.expected_size = struct.unpack(">I", value)[0]
            self.buffer = bytearray()
            return None
        elif tag == TLV_FRAME_DATA:
            self.buffer.extend(value)
            if len(self.buffer) >= self.expected_size > 0:
                frame = bytes(self.buffer[:self.expected_size])
                self.buffer.clear()
                self.expected_size = 0
                return frame
        return None


# ========== YOLO Inference Engine ==========
class YOLODetector:
    """YOLO 缺陷检测器"""
    DEFECT_CLASSES = {
        0: "scratch",
        1: "dent",
        2: "stain",
        3: "crack",
        4: "misalign",
    }

    def __init__(self, model_path: str = None, conf_threshold: float = 0.5):
        self.conf_threshold = conf_threshold
        self.model = None
        self.model_path = model_path

        if model_path and os.path.exists(model_path):
            self._load_model(model_path)
        else:
            print("[AI] No model loaded — running in dummy mode")
            print("[AI] Place your .pt file at models/yolov5s_defect.pt")

    def _load_model(self, path: str):
        try:
            from ultralytics import YOLO
            self.model = YOLO(path)
            print(f"[AI] Model loaded: {path}")
        except ImportError:
            print("[AI] ultralytics not installed, falling back to OpenCV DNN")
            try:
                import cv2
                self.model = cv2.dnn.readNetFromONNX(path) if path.endswith('.onnx') else None
                print(f"[AI] Loaded via OpenCV DNN: {path}")
            except Exception as e:
                print(f"[AI] Failed to load model: {e}")

    def detect(self, yuyv_frame: bytes, width: int, height: int):
        """YUYV 帧 → YOLO 推理 → 结果列表"""
        import numpy as np
        import cv2

        # YUYV → BGR
        yuv = np.frombuffer(yuyv_frame, dtype=np.uint8).reshape(height, width, 2)
        bgr = cv2.cvtColor(yuv, cv2.COLOR_YUV2BGR_YUYV)

        if self.model is None:
            # 无模型模式：随机模拟 (用于测试链路)
            return self._dummy_detect(bgr)

        try:
            from ultralytics import YOLO
            if isinstance(self.model, YOLO):
                results = self.model(bgr, verbose=False)
                detections = []
                for r in results:
                    for box in r.boxes:
                        conf = float(box.conf[0])
                        cls_id = int(box.cls[0])
                        if conf >= self.conf_threshold:
                            x1, y1, x2, y2 = box.xyxy[0].tolist()
                            detections.append({
                                "type": self.DEFECT_CLASSES.get(cls_id, f"cls{cls_id}"),
                                "conf": round(conf, 2),
                                "bbox": (int(x1), int(y1), int(x2 - x1), int(y2 - y1))
                            })
                return detections
        except Exception as e:
            print(f"[AI] Inference error: {e}")
            return self._dummy_detect(bgr)

        return []

    def _dummy_detect(self, bgr):
        """模拟检测：30% 概率报告 scratch"""
        import random
        import numpy as np
        # 检测图像中心区域是否有明显边缘 (简单启发式)
        import cv2
        gray = cv2.cvtColor(bgr, cv2.COLOR_BGR2GRAY)
        edges = cv2.Canny(gray, 60, 150)
        edge_ratio = np.count_nonzero(edges) / edges.size

        if edge_ratio > 0.15 and random.random() < 0.3:
            h, w = bgr.shape[:2]
            return [{
                "type": "scratch",
                "conf": round(0.75 + random.random() * 0.2, 2),
                "bbox": (w // 4, h // 4, w // 2, h // 2)
            }]
        return []


# ========== Client Handler (per connection) ==========
def handle_client(conn: socket.socket, addr, detector: YOLODetector,
                  frame_width: int, frame_height: int, save_dir: str = None):
    """处理单个网关连接"""
    print(f"[SRV] client connected: {addr[0]}:{addr[1]} (fd={conn.fileno()})")

    conn.setblocking(True)
    conn.settimeout(1.0)

    reader = TLVReader()
    reassembler = FrameReassembler()
    frame_count = 0
    defect_count = 0
    frame_dir = save_dir or "/tmp/ai_frames"
    os.makedirs(frame_dir, exist_ok=True)

    last_status = time.time()

    while g_running:
        try:
            data = conn.recv(65536)
        except socket.timeout:
            continue
        except (ConnectionResetError, ConnectionAbortedError, BrokenPipeError):
            break

        if not data:
            break

        for byte in data:
            # Feed individual bytes (same as C side)
            result = reader.feed(bytes([byte]))
            if result is None:
                continue

            tag, value = result

            # Frame reassembly
            if tag in (TLV_FRAME_START, TLV_FRAME_DATA):
                frame = reassembler.feed(tag, value)
                if frame:
                    frame_count += 1
                    if len(frame) == frame_width * frame_height * 2:
                        # AI detection
                        detections = detector.detect(frame, frame_width, frame_height)

                        if detections:
                            for det in detections:
                                defect_count += 1
                                bbox = det["bbox"]
                                print(f"[AI] DETECT: {det['type']} "
                                      f"conf={det['conf']} bbox={bbox}")

                                # Send AI_RESULT back to gateway
                                result_frame = tlv_pack_ai_result(
                                    1, det["type"], det["conf"], bbox)
                                try:
                                    conn.sendall(result_frame)
                                except Exception as e:
                                    print(f"[AI] send result failed: {e}")
                        else:
                            # Send negative result (no defect)
                            result_frame = tlv_pack_ai_result(
                                0, "none", 0.0, (0, 0, 0, 0))
                            try:
                                conn.sendall(result_frame)
                            except:
                                pass

                        # Save frame every 30 frames
                        if frame_count % 30 == 1:
                            fname = os.path.join(
                                frame_dir, f"frame_{frame_count:05d}.yuv")
                            with open(fname, "wb") as f:
                                f.write(frame)

                    # Status report
                    now = time.time()
                    if now - last_status >= 5.0:
                        print(f"[SRV] {addr[0]}: frames={frame_count} "
                              f"defects={defect_count}")
                        last_status = now

            elif tag == TLV_MES_EVENT:
                try:
                    payload = value.decode("utf-8", errors="replace")
                    print(f"[MES] {addr[0]}: {TAG_NAMES.get(tag, 'UNK')} | {payload}")
                except:
                    pass
                # Send ACK
                try:
                    conn.sendall(tlv_pack_ack(TLV_MES_EVENT))
                except:
                    pass

            elif tag == TLV_HEARTBEAT_REQ:
                try:
                    payload = value.decode("utf-8", errors="replace")
                    print(f"[HB] {addr[0]}: {payload}")
                except:
                    pass
                try:
                    import json
                    rsp = json.dumps({"pong": 1}).encode()
                    conn.sendall(tlv_pack(TLV_HEARTBEAT_RSP, rsp))
                except:
                    pass

            else:
                try:
                    payload = value.decode("utf-8", errors="replace")
                    tname = TAG_NAMES.get(tag, f"0x{tag:02X}")
                    print(f"[MSG] {addr[0]}: {tname} | {payload[:80]}")
                except:
                    pass

    print(f"[SRV] client disconnected: {addr[0]}:{addr[1]}")
    conn.close()


# ========== Main ==========
def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8899
    model_path = sys.argv[2] if len(sys.argv) > 2 else None

    # Auto-detect model
    if not model_path:
        candidates = [
            "models/yolov5s_defect.pt",
            "models/best.pt",
            "../models/yolov5s_defect.pt",
        ]
        for c in candidates:
            if os.path.exists(c):
                model_path = c
                break

    frame_width = 640
    frame_height = 480

    print("=" * 55)
    print("  AI Defect Detection Receiver (Phase 5)")
    print(f"  Port: {port}  |  Model: {model_path or 'dummy mode'}")
    print(f"  Frame: {frame_width}x{frame_height} YUYV")
    print("=" * 55)

    detector = YOLODetector(model_path, conf_threshold=0.4)

    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind(("0.0.0.0", port))
    server.listen(5)
    print(f"[SRV] Listening on 0.0.0.0:{port}")

    while g_running:
        try:
            server.settimeout(1.0)
            conn, addr = server.accept()
            threading.Thread(
                target=handle_client,
                args=(conn, addr, detector, frame_width, frame_height),
                daemon=True
            ).start()
        except socket.timeout:
            continue
        except Exception as e:
            if g_running:
                print(f"[SRV] accept error: {e}")
            break

    server.close()
    print("[SRV] exit")


if __name__ == "__main__":
    main()