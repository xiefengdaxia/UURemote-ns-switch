#pragma once
#include "protocol.hpp"
#include <SDL.h>
#include <atomic>
#include <functional>
#include <functional>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>

namespace uu {
struct Picture {
    int width = 0, height = 0;
    uint64_t serial = 0;
    Bytes pixels;
};
class Media {
    struct Impl;
    std::unique_ptr<Impl> impl_;

  public:
    Media();
    ~Media();
    void video(const uint8_t *data, size_t size, uint64_t generation);
    void audio(const uint8_t *data, size_t size, uint64_t generation);
    // RTP 序列号缺口：丢包已发生，解码器手里的参考链已断。清空队列、冻结在最后
    // 一帧、请求关键帧，避免把坏帧继续画到屏上（最多冻结 1 秒）。
    void invalidate();
    uint64_t generation() const;
    // 返回当前应显示的帧（按 vsync 节奏从小缓冲里取，共享指针避免逐帧 3.7MB 拷贝）。
    std::shared_ptr<const Picture> picture(uint64_t after);
    void reset();
    // 协商结果：切换解码器（true = H.265/HEVC）。
    void setCodec(bool hevc);
    std::string status();
    void setOverflowCallback(std::function<void()> callback);
};
} // namespace uu
