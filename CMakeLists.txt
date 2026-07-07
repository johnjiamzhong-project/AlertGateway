cmake_minimum_required(VERSION 3.16)
project(AlertGateway CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 生成 compile_commands.json，供 VSCode C/C++ 插件读取真实的 include 路径/宏定义，
# 避免 IntelliSense 因为不知道 src/ 在 include 路径里而把跨文件类型误判为不完整类型。
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ── Third-party headers ───────────────────────────────────────────────────────
set(TP ${CMAKE_SOURCE_DIR}/third_party)
include_directories(
    ${TP}/rknn/include
    ${TP}/mpp/include
    ${TP}/rga/include
    ${TP}            # nlohmann/json.hpp lives at third_party/nlohmann/json.hpp
    ${TP}/paho/include
)

# ── Sysroot ───────────────────────────────────────────────────────────────────
set(SYSROOT /home/rambos/sysroot)

# FFmpeg rockchip 6.1 headers must come BEFORE system FFmpeg 4.4.2 headers
# to match the ABI of /opt/ffmpeg-rockchip/lib/*.so on the board.
include_directories(${SYSROOT}/opt/ffmpeg-rockchip/include)
include_directories(${SYSROOT}/usr/include/aarch64-linux-gnu)

# ── Libraries (direct absolute paths) ────────────────────────────────────────
set(FFMPEG_LIB   ${SYSROOT}/opt/ffmpeg-rockchip/lib)
set(MPP_LIB      ${SYSROOT}/usr/lib/aarch64-linux-gnu/librockchip_mpp.so)
# Use the board's actual .so files as link stubs (version 1.3.9 C, 1.2.0 C++).
# Headers in third_party/paho/include are from paho-mqtt-cpp v1.2.0 to match the ABI.
set(PAHO_C_LIB   ${CMAKE_SOURCE_DIR}/third_party/paho/libpaho-mqtt3as.so.1)
set(PAHO_CPP_LIB ${CMAKE_SOURCE_DIR}/third_party/paho/libpaho-mqttpp3.so.1)

# RKNN runtime lives on the board; provide path via cmake -DRKNN_LIB_PATH=...
set(RKNN_LIB_PATH "" CACHE PATH "Path to librknnrt.so")

# RGA: use the board's actual .so as link target (2.2.0-1, matches board exactly).
set(RGA_LIB ${CMAKE_SOURCE_DIR}/third_party/rga/lib/librga.so.2.1.0)

# ── Sources ───────────────────────────────────────────────────────────────────
set(SOURCES
    src/main.cpp
    src/capture/CaptureThread.cpp
    src/infer/InferThread.cpp
    src/infer/RockchipYoloPostprocess.cpp
    src/infer/YoloPostprocess.cpp
    src/encode/EncodeThread.cpp
    src/stream/StreamThread.cpp
    src/mqtt/MqttThread.cpp
)

add_executable(AlertGateway ${SOURCES})

target_include_directories(AlertGateway PRIVATE ${CMAKE_SOURCE_DIR}/src)

# RKNN: use stub .so for cross-link; real librknnrt.so is deployed on the board.
set(RKNN_STUB ${CMAKE_SOURCE_DIR}/third_party/rknn/librknnrt.so)

target_link_libraries(AlertGateway PRIVATE
    ${RKNN_STUB}
    ${MPP_LIB}
    ${RGA_LIB}
    ${PAHO_C_LIB}
    ${PAHO_CPP_LIB}
    ${FFMPEG_LIB}/libavformat.so
    ${FFMPEG_LIB}/libavcodec.so
    ${FFMPEG_LIB}/libavutil.so
    pthread
)

# --allow-shlib-undefined: FFmpeg/Paho transitive deps (libssl, libva, libx264, etc.)
# are present on the board but not in the cross-compile sysroot.
target_link_options(AlertGateway PRIVATE -Wl,--allow-shlib-undefined)

# Standalone RKNN microbenchmark used for controlled model comparisons.
# Disabled by default so the production build remains unchanged.
option(BUILD_RKNN_BENCHMARK "Build the standalone RKNN benchmark" OFF)
if(BUILD_RKNN_BENCHMARK)
    add_executable(rknn_benchmark
        tools/benchmark/rknn_benchmark.cpp
        src/infer/RockchipYoloPostprocess.cpp
        src/infer/YoloPostprocess.cpp
    )
    target_include_directories(rknn_benchmark PRIVATE
        ${TP}/rknn/include
        ${CMAKE_SOURCE_DIR}/src
    )
    target_link_libraries(rknn_benchmark PRIVATE ${RKNN_STUB} pthread)
endif()

# Camera + production InferThread smoke test without RTMP/MQTT dependencies.
option(BUILD_INFER_CAMERA_SMOKE "Build the camera inference smoke test" OFF)
if(BUILD_INFER_CAMERA_SMOKE)
    add_executable(infer_camera_smoke
        tools/benchmark/infer_camera_smoke.cpp
        src/capture/CaptureThread.cpp
        src/infer/InferThread.cpp
        src/infer/RockchipYoloPostprocess.cpp
        src/infer/YoloPostprocess.cpp
    )
    target_include_directories(infer_camera_smoke PRIVATE
        ${TP}/rknn/include
        ${TP}/rga/include
        ${TP}
        ${CMAKE_SOURCE_DIR}/src
    )
    target_link_libraries(infer_camera_smoke PRIVATE
        ${RKNN_STUB}
        ${RGA_LIB}
        pthread
    )
endif()
