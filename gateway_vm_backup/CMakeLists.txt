cmake_minimum_required(VERSION 3.10)
project(edge_gateway C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 11)

# --- Phase 1: Basic ---
add_executable(hello     src/main.c)
add_executable(gpio_test src/gpio_led.c)
add_executable(cam_test  src/camera_test.c)

# --- Phase 2: I2C Driver & GPIO ---
add_executable(bh1750_test     src/bh1750_test.c)
add_executable(peripheral_ctrl src/peripheral_ctrl.c src/gpio_ops.c)

# --- Phase 3: TLV Protocol ---
set(TLV_SRCS src/tlv_protocol.c src/mes_handler.c)

add_executable(tcp_server src/tcp_server.c ${TLV_SRCS})
add_executable(tcp_client src/tcp_client.c ${TLV_SRCS})

# --- Phase 4: V4L2 Video + Multi-thread ---
set(VCAP_SRCS src/video_capture.c src/frame_sender.c src/gpio_ops.c ${TLV_SRCS})

add_executable(gateway_app src/gateway_app.cpp ${VCAP_SRCS})
target_link_libraries(gateway_app pthread)

add_executable(frame_receiver src/frame_receiver.c ${TLV_SRCS})