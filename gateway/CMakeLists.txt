cmake_minimum_required(VERSION 3.10)
project(edge_gateway C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 11)

# --- 阶段1: 基础验证 ---
add_executable(hello     src/main.c)
add_executable(gpio_test src/gpio_led.c)
add_executable(cam_test  src/camera_test.c)

# --- 阶段2: I2C驱动测试 & GPIO外设 ---
add_executable(bh1750_test     src/bh1750_test.c)
add_executable(peripheral_ctrl src/peripheral_ctrl.c) src/gpio_ops.c

# --- 阶段3: TLV 通信引擎 ---
set(TLV_SRCS src/tlv_protocol.c src/mes_handler.c)

# VM 本地编译 (tcp_server)
add_executable(tcp_server src/tcp_server.c ${TLV_SRCS})

# ARM 交叉编译 (tcp_client)
add_executable(tcp_client src/tcp_client.c ${TLV_SRCS})

# --- 阶段4: V4L2 视频 + 多线程 ---
set(VCAP_SRCS src/video_capture.c src/frame_sender.c src/gpio_ops.c ${TLV_SRCS})

# ARM 交叉编译 (gateway_app) --- C++11 多线程
add_executable(gateway_app src/gateway_app.cpp ${VCAP_SRCS})
target_link_libraries(gateway_app pthread)

# VM 本地编译 (frame_receiver)
add_executable(frame_receiver src/frame_receiver.c ${TLV_SRCS})
