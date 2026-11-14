#include "client.hpp"
#include "background.hpp"
#include "diagnostics.hpp"
#include "event_queue.hpp"
#include "rtc_diagnostics.hpp"
#include "rtc_signaling.hpp"
#include "signal_pump.hpp"
#include <chrono>
#include <cstdio>
#include <fstream>
#include <stdexcept>

namespace uu {
namespace {
Json loadSession(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return {};
    auto saved = Json::parse(std::string(std::istreambuf_iterator<char>(f), {}), nullptr, false);
    if (saved.is_discarded() || !saved.is_object() || !saved.contains("token") ||
        !saved.contains("userId"))
        return {};
    if (saved.value("token", std::string{}).empty() || saved.value("userId", std::string{}).empty())
        return {};
    return saved;
}
// 只记录协商能力相关的行（编码/扩展/反馈/方向），不含 ICE 候选等敏感信息。
void dumpAnswerSdp(const std::string &sdp) {
    size_t start = 0;
    while (start < sdp.size()) {
        const size_t end = sdp.find('\n', start);
        std::string line =
            sdp.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        static const char *const prefixes[] = {
            "m=",       "a=rtpmap", "a=fmtp", "a=rtcp-fb",   "a=extmap", "a=mid",
            "a=sctp-port", "a=setup", "a=send", "a=recv",    "a=inactive"};
        for (const auto *prefix : prefixes) {
            if (line.rfind(prefix, 0) == 0) {
                diagnostic(("sdp-ans|" + line).c_str());
                break;
            }
        }
        if (end == std::string::npos)
            break;
        start = end + 1;
    }
}
} // namespace

// libdatachannel 的 RtcpPli 把「packet sender SSRC」也填成对端媒体 SSRC，
// 部分发送端会因此忽略请求；这里用本端 SSRC 重发 PLI + FIR 双保险。
class KeyframeRequestHandler final : public rtc::MediaHandler {
  public:
    void incoming(rtc::message_vector &messages, const rtc::message_callback &) override {
        for (auto &message : messages) {
            if (message->type != rtc::Message::Type::Binary || message->size() < 12)
                continue;
            auto *rtp = reinterpret_cast<const uint8_t *>(message->data());
            if ((rtp[0] >> 6) != 2)
                continue;
            mediaSsrc_ = (uint32_t(rtp[8]) << 24) | (uint32_t(rtp[9]) << 16) |
                         (uint32_t(rtp[10]) << 8) | uint32_t(rtp[11]);
        }
    }
    bool requestKeyframe(const rtc::message_callback &send) override {
        if (mediaSsrc_) {
            const int64_t now = monotonicMs();
            if (now - lastRequestMs_ >= 300) {
                lastRequestMs_ = now;
                sendControl(send, buildPli());
                sendControl(send, buildFir());
            }
        }
        return false; // 继续沿链交给 rtcpSession
    }

  private:
    static constexpr uint32_t kLocalSsrc = 0x4B4C4643;
    static void write32(uint8_t *at, uint32_t value) {
        at[0] = uint8_t(value >> 24);
        at[1] = uint8_t(value >> 16);
        at[2] = uint8_t(value >> 8);
        at[3] = uint8_t(value);
    }
    void sendControl(const rtc::message_callback &send, const std::vector<uint8_t> &p) {
        try {
            send(std::make_shared<rtc::Message>(
                reinterpret_cast<const std::byte *>(p.data()),
                reinterpret_cast<const std::byte *>(p.data()) + p.size(),
                rtc::Message::Type::Control));
        } catch (...) {
        }
    }
    std::vector<uint8_t> buildPli() const {
        std::vector<uint8_t> p(12, 0);
        p[0] = 0x81; // V=2, FMT=1
        p[1] = 206;  // PSFB
        p[3] = 2;    // 12 bytes
        write32(&p[4], kLocalSsrc);
        write32(&p[8], mediaSsrc_);
        return p;
    }
    std::vector<uint8_t> buildFir() {
        std::vector<uint8_t> p(20, 0);
        p[0] = 0x84; // V=2, FMT=4
        p[1] = 206;  // PSFB
        p[3] = 4;    // 20 bytes
        write32(&p[4], kLocalSsrc);
        write32(&p[12], mediaSsrc_);
        p[16] = firSeq_++;
        return p;
    }
    uint32_t mediaSsrc_ = 0;
    int64_t lastRequestMs_ = 0;
    uint8_t firSeq_ = 1;
};

// transport-cc (draft-holmer) 接收端反馈：官方引擎是 libwebrtc 魔改，其 GCC 带宽估计
// 依赖接收端周期回传 transport-wide 序列号到达时间；libdatachannel 默认不发，
// 缺失反馈时对端码率估计趴在保守默认值上（实测 ~2.5Mbps 封顶）。
class TransportCcHandler final : public rtc::MediaHandler {
  public:
    void setOnGap(std::function<void()> callback) { onGap_ = std::move(callback); }
    void setPayloadTypeSink(std::atomic<int> *sink) { ptSink_ = sink; }

    void incoming(rtc::message_vector &messages, const rtc::message_callback &send) override {
        auto now = steadyUs();
        int rtpCount = 0;
        for (auto &message : messages) {
            if (message->type != rtc::Message::Type::Binary || message->size() < 20)
                continue;
            const auto *head = reinterpret_cast<const uint8_t *>(message->data());
            if ((head[0] >> 6) == 2)
                ++rtpCount;
        }
        // 合成到达时钟：视频是突发到达的（一帧几十上百个包），而 Wi-Fi AP 的缓冲
        // 抖动会让 GCC 的延迟判据反复误判成拥塞——实测同一会话里码率在 0.6M 与
        // 3.5M 之间锯齿状震荡。这里保持平均速率真实（EWMA 间隔），但去掉突发抖动，
        // 让服务器的带宽估计稳定在画质档目标上。
        if (rtpCount) {
            if (lastRealUs_ && now > lastRealUs_) {
                const int64_t elapsed = now - lastRealUs_;
                if (elapsed < 500000) {
                    const int64_t perPacket = std::max<int64_t>(elapsed / rtpCount, 50);
                    intervalUs_ = intervalUs_ ? (intervalUs_ * 7 + perPacket) / 8 : perPacket;
                }
            }
            lastRealUs_ = now;
        }
        const int64_t stepUs = intervalUs_ ? intervalUs_ : 1000;
        // 每个批次以真实时刻为锚点（不累积漂移），批内按平滑后的包间隔均匀铺开，
        // 去掉突发造成的锯齿，但平均速率仍真实。
        const int64_t span = std::min<int64_t>(stepUs * rtpCount, 25000);
        const int64_t origin = now - span;
        int rtpIndex = 0;
        std::lock_guard lock(mutex_);
        for (auto &message : messages) {
            if (message->type != rtc::Message::Type::Binary || message->size() < 20)
                continue;
            auto *rtp = reinterpret_cast<const uint8_t *>(message->data());
            if ((rtp[0] >> 6) != 2)
                continue; // 非 RTP
            const int64_t arrivalUs =
                rtpCount ? origin + span * (++rtpIndex) / rtpCount : now;
            ++intervalPackets_;
            intervalBytes_ += message->size();
            if (ptSink_)
                ptSink_->store(int(rtp[1] & 0x7F), std::memory_order_relaxed);
            if (!ptLogged_) {
                ptLogged_ = true;
                char ptLine[24];
                std::snprintf(ptLine, sizeof ptLine, "rtp-pt-%u", unsigned(rtp[1] & 0x7F));
                diagnostic(ptLine);
            }
            const uint32_t rtpTimestamp = (uint32_t(rtp[4]) << 24) | (uint32_t(rtp[5]) << 16) |
                                          (uint32_t(rtp[6]) << 8) | uint32_t(rtp[7]);
            if (rtpTimestamp != lastTimestamp_) {
                lastTimestamp_ = rtpTimestamp;
                ++intervalFrames_;
            }
            // RR 用：跟踪 RTP 序列号最高值（含回绕累计）
            uint16_t rtpSeq = uint16_t(rtp[2] << 8) | rtp[3];
            if (!hasRtpSeq_) {
                extendedHighest_ = rtpSeq;
                firstSeq_ = rtpSeq;
                hasRtpSeq_ = true;
                receivedDistinct_ = 1;
            } else {
                uint16_t delta = uint16_t(rtpSeq - uint16_t(extendedHighest_));
                if (delta && delta < 0x8000) {
                    if (delta > 1) {
                        // 序列号缺口 = 丢包：先发 NACK 请求重传（100ms 节流），
                        // 再请求关键帧兜底（500ms 节流）。
                        cumulativeLost_ += delta - 1;
                        if (now - lastNackUs_ > 100000) {
                            lastNackUs_ = now;
                            // RTCP 205 FMT=1 Generic NACK：PID + BLP 覆盖整个缺口
                            uint16_t firstLost = uint16_t(extendedHighest_ + 1);
                            size_t lostCount = size_t(delta - 1);
                            size_t entries = (lostCount + 15) / 16;
                            size_t body = 12 + entries * 4;
                            size_t padded = (body + 3) & ~size_t(3);
                            std::vector<uint8_t> p(padded, 0);
                            p[0] = 0x81; // V=2, FMT=1
                            p[1] = 205;
                            uint32_t words = uint32_t(padded / 4) - 1;
                            p[2] = uint8_t(words >> 8);
                            p[3] = uint8_t(words);
                            p[4] = 0x4B;
                            p[5] = 0x4C;
                            p[6] = 0x46;
                            p[7] = 0x43;
                            if (mediaSsrc_) {
                                p[8] = uint8_t(mediaSsrc_ >> 24);
                                p[9] = uint8_t(mediaSsrc_ >> 16);
                                p[10] = uint8_t(mediaSsrc_ >> 8);
                                p[11] = uint8_t(mediaSsrc_);
                            }
                            for (size_t i = 0; i < entries; ++i) {
                                uint16_t pid = uint16_t(firstLost + 16 * i);
                                size_t at = 12 + i * 4;
                                p[at] = uint8_t(pid >> 8);
                                p[at + 1] = uint8_t(pid);
                                size_t remaining = lostCount - 16 * i;
                                uint16_t blp = uint16_t(remaining >= 16 ? 0xFFFF
                                                                        : (uint16_t(1 << (remaining - 1)) - 1) << 1 |
                                                                              (remaining >= 2 ? 1 : 0));
                                // remaining 个后续包全部丢失：bit0..bit(remaining-2)
                                blp = 0;
                                for (size_t k = 1; k < remaining && k <= 16; ++k)
                                    blp |= uint16_t(1 << (k - 1));
                                p[at + 2] = uint8_t(blp >> 8);
                                p[at + 3] = uint8_t(blp);
                            }
                            try {
                                auto nack = std::make_shared<rtc::Message>(
                                    reinterpret_cast<const std::byte *>(p.data()),
                                    reinterpret_cast<const std::byte *>(p.data()) + p.size(),
                                    rtc::Message::Type::Control);
                                send(nack);
                                diagnostic(("nack-sent-" + std::to_string(delta - 1)).c_str());
                            } catch (...) {
                                diagnostic("nack-send-fail");
                            }
                        }
                        // 任何缺口都必须修复：小缺口不修的话会残留到下一个自然关键帧
                        //（最长十几秒），表现为持续的竖线/局部糊。PLI 节流到 1 秒一次，
                        // 单个整帧 IDR 在 8M 档只占 3~4% 码率，可以接受。
                        if (onGap_ && now - lastInvalidateUs_ > 1000000) {
                            lastInvalidateUs_ = now;
                            onGap_();
                        }
                        if (now - lastPliUs_ > 500000) {
                            lastPliUs_ = now;
                            diagnostic(("rtp-gap-" + std::to_string(delta - 1)).c_str());
                        }
                    }
                    extendedHighest_ += delta;
                    receivedDistinct_++;
                } else if (delta) {
                    ++latePackets_; // 乱序/重传包：若服务器重传生效，这里会增长
                }
            }
            if (!(rtp[0] & 0x10))
                continue; // 无扩展头（TWCC 序列号不存在）
            unsigned csrc = rtp[0] & 0x0F;
            size_t pos = 12 + 4 * csrc;
            if (pos + 4 > message->size())
                continue;
            uint16_t profile = uint16_t(rtp[pos] << 8) | rtp[pos + 1];
            uint16_t words = uint16_t(rtp[pos + 2] << 8) | rtp[pos + 3];
            if (profile != 0xBEDE)
                continue; // 只认 one-byte header 扩展
            size_t end = std::min<size_t>(pos + 4 + 4 * words, message->size());
            pos += 4;
            long twcc = -1;
            while (pos + 1 <= end) {
                unsigned id = rtp[pos] >> 4, len = (rtp[pos] & 0x0F) + 1;
                if (pos + 1 + len > end)
                    break;
                if ((id == 1 || id == 2) && len == 2) // offer 里声明的 extmap id
                    twcc = (rtp[pos + 1] << 8) | rtp[pos + 2];
                pos += 1 + len;
            }
            if (twcc < 0) {
                if (!extChecked_) {
                    extChecked_ = true;
                    diagnostic("twcc-ext-off");
                }
                continue;
            }
            if (!extChecked_) {
                extChecked_ = true;
                diagnostic("twcc-ext-on");
            }
            uint32_t ssrc = (uint32_t(rtp[8]) << 24) | (uint32_t(rtp[9]) << 16) |
                            (uint32_t(rtp[10]) << 8) | rtp[11];
            if (ssrc != mediaSsrc_) { // 新会话或 SSRC 变化：重置窗口
                mediaSsrc_ = ssrc;
                pending_.clear();
                hasPending_ = false;
                hasRtpSeq_ = false;
                extendedHighest_ = rtpSeq;
                hasRtpSeq_ = true;
            }
            if (hasPending_) {
                unsigned advance = unsigned(twcc - lastTwcc_) & 0xFFFF;
                if (advance == 0 || advance > 0x7FFF) // 重传/乱序：不进反馈窗口
                    continue;
            }
            pending_.push_back({uint16_t(twcc), arrivalUs});
            if (pending_.size() > kMaxPending)
                pending_.pop_front();
            hasPending_ = true;
            lastTwcc_ = twcc;
        }
        // 每 5 秒记录实际接收码率/包率/丢包，判断服务器是否真的在发满速。
        if (!lastStatsUs_)
            lastStatsUs_ = now;
        if (now - lastStatsUs_ >= 5000000) {
            const double seconds = double(now - lastStatsUs_) / 1e6;
            const unsigned kbps = unsigned(double(intervalBytes_) * 8.0 / seconds / 1000.0);
            const unsigned pps = unsigned(double(intervalPackets_) / seconds);
            char line[96];
            std::snprintf(line, sizeof line, "rx-%ukbps-%upps-lost%u-f%u-late%u", kbps, pps,
                          unsigned(cumulativeLost_ - intervalLostStart_),
                          unsigned(intervalFrames_), unsigned(latePackets_ - intervalLateStart_));
            diagnostic(line);
            intervalPackets_ = intervalBytes_ = intervalFrames_ = 0;
            intervalLostStart_ = cumulativeLost_;
            intervalLateStart_ = latePackets_;
            lastStatsUs_ = now;
        }
    }

    bool requestBitrate(unsigned int bitrate, const rtc::message_callback &send) override {
        rtc::message_vector out;
        {
            std::lock_guard lock(mutex_);
            buildFeedback(out);
            buildReceiverReport(out);
        }
        for (auto &m : out) {
            try {
                send(m);
            } catch (...) {
                diagnostic("tcc-send-failed");
            }
        }
        return MediaHandler::requestBitrate(bitrate, send); // 链上后续 handler 继续发 REMB
    }

  private:
    struct Entry {
        uint16_t seq;
        int64_t arrivalUs;
    };
    static constexpr size_t kMaxPending = 400;
    static int64_t steadyUs() {
        return std::chrono::duration_cast<std::chrono::microseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }
    // TWCC 反馈（draft-holmer）：
    //  - reference time 单位是 64ms（24bit）。此前误用 6.25ms，把发送端算出的
    //    一路延迟放大约 10 倍，GCC 判为持续过载，码率被压到几百 kbps。
    //  - run-length chunk 必须是"同状态连续段"：缺口要显式编码成 S=0（未收到），
    //    否则发送端会把丢掉的包当成已收到，delta 流整体错位。
    //  - 每个收到包都带 2 字节 delta（250us 单位，S=2 大 delta）；
    //    第一个 delta 相对 reference time。
    void buildFeedback(rtc::message_vector &out) {
        if (pending_.empty() || !mediaSsrc_)
            return;
        const uint16_t base = pending_.front().seq;
        const size_t count = size_t(uint16_t(pending_.back().seq - base)) + 1;
        if (count > 2048) { // 序列号跳变或长时间停顿：丢弃窗口，避免编码异常
            pending_.clear();
            hasPending_ = false;
            return;
        }
        std::vector<int64_t> arrivals(count, -1);
        for (auto &e : pending_) {
            const size_t at = size_t(uint16_t(e.seq - base));
            if (at < count)
                arrivals[at] = e.arrivalUs;
        }
        // 实测链路 42Mbps，而视频只跑 4M——这里的缺口是 Wi-Fi 随机干扰，不是拥塞。
        // 服务器却把丢包当拥塞，降码率+降帧率（实测稳定 75 秒后突然 4M→0.9M/33fps）。
        // 因此把缺口按"已收到"上报（到达时间沿用前一个包），让它的速率控制保持稳定；
        // 真实恢复仍由客户端自己的 PLI/FIR 完成。
        {
            int64_t lastValid = pending_.front().arrivalUs;
            for (size_t i = 0; i < count; ++i) {
                if (arrivals[i] >= 0)
                    lastValid = arrivals[i];
                else
                    arrivals[i] = lastValid;
            }
        }
        std::vector<uint16_t> chunks;
        for (size_t i = 0; i < count;) {
            const bool received = arrivals[i] >= 0;
            size_t run = 1;
            while (i + run < count && (arrivals[i + run] >= 0) == received && run < 0x1FFF)
                ++run;
            chunks.push_back(uint16_t((received ? (2u << 13) : 0u) | uint16_t(run)));
            i += run;
        }
        std::vector<uint8_t> deltas;
        int64_t prevUs = 0;
        bool first = true;
        for (size_t i = 0; i < count; ++i) {
            if (arrivals[i] < 0)
                continue;
            const int64_t d = first ? arrivals[i] - (arrivals[i] / 64000) * 64000
                                    : arrivals[i] - prevUs;
            first = false;
            prevUs = arrivals[i];
            const int32_t units = int32_t(std::clamp<int64_t>(d / 250, -32768, 32767));
            deltas.push_back(uint8_t(uint16_t(units) >> 8));
            deltas.push_back(uint8_t(uint16_t(units)));
        }
        const uint32_t ref = uint32_t(pending_.front().arrivalUs / 64000) & 0xFFFFFF;
        const size_t body = 20 + 2 * chunks.size() + deltas.size();
        const size_t padded = (body + 3) & ~size_t(3);
        std::vector<uint8_t> p(padded, 0);
        p[0] = 0x80 | 15; // V=2, FMT=15
        if (padded > body)
            p[0] |= 0x20; // P
        p[1] = 205;       // Transport Specific Feedback
        uint32_t words = uint32_t(padded / 4) - 1;
        p[2] = uint8_t(words >> 8);
        p[3] = uint8_t(words);
        p[4] = 0x4B;
        p[5] = 0x4C;
        p[6] = 0x46;
        p[7] = 0x43; // sender SSRC（本地固定值）
        p[8] = uint8_t(mediaSsrc_ >> 24);
        p[9] = uint8_t(mediaSsrc_ >> 16);
        p[10] = uint8_t(mediaSsrc_ >> 8);
        p[11] = uint8_t(mediaSsrc_);
        p[12] = uint8_t(base >> 8);
        p[13] = uint8_t(base);
        p[14] = uint8_t(count >> 8);
        p[15] = uint8_t(count);
        p[16] = uint8_t(ref >> 16);
        p[17] = uint8_t(ref >> 8);
        p[18] = uint8_t(ref);
        p[19] = feedbackCount_++ & 0xFF;
        size_t at = 20;
        for (auto chunk : chunks) {
            p[at++] = uint8_t(chunk >> 8);
            p[at++] = uint8_t(chunk);
        }
        for (auto byte : deltas)
            p[at++] = byte;
        if (padded > body)
            p[padded - 1] = uint8_t(padded - body);
        out.push_back(std::make_shared<rtc::Message>(reinterpret_cast<const std::byte *>(p.data()),
                                                    reinterpret_cast<const std::byte *>(p.data()) + p.size(),
                                                    rtc::Message::Type::Control));
        pending_.clear();
        hasPending_ = false;
    }

    // RTCP RR：零丢包 + 精确最高序列号。libwebrtc 系发送端的 loss-based 带宽控制
    // 以 RR 为输入；libdatachannel 默认不发，服务器可能因此按保守策略压码率。
    void buildReceiverReport(rtc::message_vector &out) {
        if (!mediaSsrc_ || !hasRtpSeq_)
            return;
        std::vector<uint8_t> p(32, 0);
        p[0] = 0x81; // V=2, RC=1
        p[1] = 201;  // Receiver Report
        p[2] = 0;
        p[3] = 7; // 8 words - 1
        p[4] = 0x4B;
        p[5] = 0x4C;
        p[6] = 0x46;
        p[7] = 0x43; // sender SSRC（本地固定值）
        p[8] = uint8_t(mediaSsrc_ >> 24);
        p[9] = uint8_t(mediaSsrc_ >> 16);
        p[10] = uint8_t(mediaSsrc_ >> 8);
        p[11] = uint8_t(mediaSsrc_);
        uint32_t expected = extendedHighest_ - firstSeq_;
        // fraction 与累计丢包都报 0：与 TWCC 一致（丢包是 Wi-Fi 干扰，不该触发降档），
        // 否则发送端的 loss-based 控制器会周期性把码率/帧率压下去。
        p[12] = 0;
        p[13] = 0;
        p[14] = 0;
        p[15] = 0;
        lastRrExpected_ = expected;
        lastRrLost_ = cumulativeLost_;
        p[16] = uint8_t(extendedHighest_ >> 24);
        p[17] = uint8_t(extendedHighest_ >> 16);
        p[18] = uint8_t(extendedHighest_ >> 8);
        p[19] = uint8_t(extendedHighest_);
        // jitter / LSR / DLSR 保持 0
        out.push_back(std::make_shared<rtc::Message>(reinterpret_cast<const std::byte *>(p.data()),
                                                    reinterpret_cast<const std::byte *>(p.data()) + p.size(),
                                                    rtc::Message::Type::Control));
    }

    std::mutex mutex_;
    std::deque<Entry> pending_;
    uint32_t extendedHighest_ = 0, firstSeq_ = 0, cumulativeLost_ = 0;
    uint32_t receivedDistinct_ = 0, lastRrExpected_ = 0, lastRrLost_ = 0;
    std::function<void()> onGap_;
    int64_t lastPliUs_ = 0;
    int64_t lastNackUs_ = 0;
    int64_t lastInvalidateUs_ = 0;
    uint64_t intervalPackets_ = 0, intervalBytes_ = 0, intervalFrames_ = 0;
    uint64_t latePackets_ = 0, intervalLateStart_ = 0;
    uint32_t lastTimestamp_ = 0;
    uint32_t intervalLostStart_ = 0;
    int64_t lastStatsUs_ = 0;
    int64_t intervalUs_ = 0, lastRealUs_ = 0;
    bool hasRtpSeq_ = false;
    uint32_t mediaSsrc_ = 0;
    uint16_t lastTwcc_ = 0;
    bool hasPending_ = false, extChecked_ = false, ptLogged_ = false;
    std::atomic<int> *ptSink_ = nullptr;
    uint8_t feedbackCount_ = 0;
};

Client::Client(std::string key, std::string client, std::shared_ptr<Media> media,
               std::string sessionPath)
    : media_(std::move(media)), sessionPath_(std::move(sessionPath)) {
    api_ = std::make_unique<Api>(std::move(key), std::move(client), cancel_);
    worker_ = std::thread([this] { run(); });
}
Client::~Client() {
    stop_ = true;
    cancel_ = true;
    inbox_->cv.notify_all();
    worker_.join();
}
Snapshot Client::snapshot() {
    std::lock_guard lock(stateMutex_);
    return state_;
}
void Client::setState(const std::string &stage, const std::string &detail) {
    diagnostic(stage.c_str());
    std::lock_guard lock(stateMutex_);
    state_.stage = stage;
    state_.detail = detail;
    state_.streaming = stage == "connected";
    state_.authenticated = api_->authenticated();
}
void Client::post(std::string type, Json data, uint64_t generation) {
    std::lock_guard lock(inbox_->mutex);
    inbox_->events.push_back({std::move(type), std::move(data), generation});
    inbox_->cv.notify_one();
}
void Client::command(const std::string &name, Json data) {
    if (name == "disconnect" || name == "logout" || name == "cancel" || name == "sms-open" || name == "login")
        cancel_ = true;
    post(name, std::move(data));
}
void Client::drainEvents() {
    drainEventBatch(
        inbox_->events, inbox_->mutex, [this] { return stop_.load(); },
        [this](const Event &e) {
            if (e.generation && e.generation != generation_)
                return;
            handle(e);
        });
}
void Client::run() {
    if (!sessionPath_.empty())
        tryRestore();
    while (!stop_) {
        try {
            {
                std::unique_lock lock(inbox_->mutex);
                if (inbox_->events.empty())
                    inbox_->cv.wait_for(lock, std::chrono::milliseconds(4));
            }
            drainEvents();
            if (stop_)
                break;
            if (socket_) {
                pumpSignaling(
                    [this](WsMessage &m) {
                        const auto start = monotonicMs();
                        const bool received = socket_->receive(m);
                        const auto elapsed = monotonicMs() - start;
                        if (elapsed >= 100)
                            diagnostic(("signal-read-slow-ms-" + std::to_string(elapsed)).c_str());
                        return received;
                    },
                    [this](const WsMessage &m) { packet(m); },
                    [this] {
                        if (stop_ || cancel_)
                            return true;
                        std::lock_guard lock(inbox_->mutex);
                        return !inbox_->events.empty();
                    },
                    [] { return monotonicMs(); });
                // Connected/failed callbacks may arrive while processing SDP/ICE.
                // Apply them before the deadline, including callbacks queued at its boundary.
                drainEvents();
                if (socket_ && monotonicMs() - lastPacket_ > heartbeatTimeout_)
                    throw std::runtime_error("信令心跳超时");
            }
            if (deadline_ && monotonicMs() > deadline_) {
                if (peer_) {
                    diagnostic(("timeout-peer-state-" + std::to_string(int(peer_->state()))).c_str());
                    diagnostic(("timeout-ice-state-" + std::to_string(int(peer_->iceState()))).c_str());
                }
                throw std::runtime_error("连接超时，请检查电脑和网络后重试");
            }
            if (!rpcSent_) { // 数据通道在 Peer 连接后才就绪，持续重试直到发出
                sendRpcSettings();
            }
            if (pendingEchoSeq_.load() != 0) {
                auto it = channels_.find("CONTROL_DATA_CHANNEL");
                if (it != channels_.end() && it->second->isOpen()) {
                    try {
                        auto reply = echoResponse(nextSeq_++, pendingEchoSeq_.load(),
                                                  sessionFeatures());
                        it->second->send(reinterpret_cast<const std::byte *>(reply.data()),
                                         reply.size());
                        pendingEchoSeq_ = 0;
                        if (!echoReplied_) {
                            echoReplied_ = true;
                            diagnostic("echo-replied");
                        }
                    } catch (...) {
                    }
                }
            }
            {
                auto it = channels_.find("CONTROL_DATA_CHANNEL");
                if (it != channels_.end() && it->second->isOpen() &&
                    monotonicMs() - lastEchoMs_ >= 2000) {
                    try {
                        auto ping = echoRequest(nextSeq_++, sessionFeatures());
                        it->second->send(reinterpret_cast<const std::byte *>(ping.data()),
                                         ping.size());
                    } catch (...) {
                    }
                    lastEchoMs_ = monotonicMs();
                }
            }
            // 按实际 RTP payload type 选择解码器：SDP 里双方都声明了 H265，
            // 但服务器实际发哪一路只能看包（实测 PT 96 = H.264）。
            const int videoPt = videoPt_.load(std::memory_order_relaxed);
            if (video_ && videoPt >= 0 && (videoPt == 97) != videoHevc_) {
                videoHevc_ = videoPt == 97;
                media_->setCodec(videoHevc_);
                if (attachVideo_)
                    attachVideo_(videoHevc_);
                try {
                    video_->requestKeyframe();
                } catch (...) {
                }
            }
            if (video_ && video_->isOpen() &&
                (iceState_ == rtc::PeerConnection::IceState::Connected ||
                 iceState_ == rtc::PeerConnection::IceState::Completed) &&
                monotonicMs() - lastFeedback_ >= 100) {
                // 触发视频链上的 transport-cc 反馈（draft-holmer），并保留 REMB。
                // libwebrtc 的 GCC 默认按 100ms 周期回报，300ms 会让估计收敛过慢。
                // 任何发送失败都不能中断连接。
                unsigned remb = 20000000;
                if (reqQuality_ == 1 || reqQuality_ == 2)
                    remb = 2000000;
                else if (reqQuality_ == 3)
                    remb = 8000000;
                else if (reqQuality_ == 6)
                    remb = unsigned(reqCustomBps_);
                try {
                    video_->requestBitrate(remb);
                } catch (...) {
                    diagnostic("feedback-send-failed");
                }
                lastFeedback_ = monotonicMs();
            }
            if (!qr_.is_null() && monotonicMs() >= qrNext_)
                pollQr();
        } catch (const std::exception &error) {
            if (stop_)
                break;
            const auto failure = connectionFailure(snapshot().stage, error.what());
            diagnosticException("client-exception", std::current_exception());
            const auto reason = rtcLogEvent(error.what());
            if (!reason.empty())
                diagnostic(reason.c_str());
            disconnect();
            if (cancel_) {
                cancel_ = false;
                setState("idle", "已取消");
            } else
                setState("error", failure);
        } catch (...) {
            diagnostic("client-unknown-exception");
            disconnect();
            cancel_ = false;
            setState("error", "发生异常，连接阶段已写入 connection.log");
        }
    }
    cancel_ = false;
    disconnect();
}
void Client::handle(const Event &event) {
    const auto &e = event.type;
    const auto &data = event.data;
    if (e == "login") {
        disconnect();
        cancel_ = false;
        {
            std::lock_guard lock(stateMutex_);
            state_.loginMode = "qr";
        }
        setState("login", "正在生成登录二维码…");
        api_->initialize();
        qr_ = api_->request("POST", "/api/v1/qrcode/gen/login");
        if (!qr_.is_object() || !qr_.contains("token") || !qr_.contains("qrcode_jump_url"))
            throw std::runtime_error("登录二维码响应不完整");
        qrExpires_ = monotonicMs() + 180000;
        qrNext_ = monotonicMs() + 1000;
        qrStatus_ = 1;
        {
            std::lock_guard lock(stateMutex_);
            state_.qrUrl = qr_["qrcode_jump_url"].get<std::string>();
        }
        setState("login", "请使用手机 UU远程 App 扫码并确认");
    } else if (e == "sms-open") {
        disconnect();
        cancel_ = false;
        {
            std::lock_guard lock(stateMutex_);
            state_.loginMode = "sms";
        }
        setState("sms", "输入手机号并发送验证码");
    } else if (e == "sms-code" || e == "sms-login") {
        cancel_ = false;
        auto request = [this](const std::string &path, const Json &body) {
            api_->initialize();
            return api_->request("POST", path, body);
        };
        if (e == "sms-code") {
            setState("sms-sending", "正在发送短信验证码…");
            sms_.sendCode(request, data, monotonicMs());
            {
                std::lock_guard lock(stateMutex_);
                state_.smsRetryAt = sms_.retryAt();
            }
            setState("sms", "验证码已发送，输入 6 位验证码后登录");
        } else {
            setState("sms-verifying", "正在验证短信验证码…");
            completeLogin(sms_.login(request, data));
        }
    } else if (e == "cancel" || e == "disconnect") {
        disconnect();
        cancel_ = false;
        {
            std::lock_guard lock(stateMutex_);
            state_.loginMode = "qr";
        }
        setState("idle", "已断开，按 Y 刷新设备或按 A 登录");
    } else if (e == "logout") {
        disconnect();
        cancel_ = false;
        api_->logout();
        if (!sessionPath_.empty())
            std::remove(sessionPath_.c_str());
        {
            std::lock_guard lock(stateMutex_);
            state_.devices = Json::array();
            state_.nickname.clear();
        }
        setState("idle", "已退出账号，按 A 重新扫码");
    } else if (e == "refresh") {
        if (api_->authenticated()) {
            cancel_ = false;
            setState("loading", "正在同步设备…");
            refresh();
        } else
            setState("idle", "请先按 A 扫码登录");
    } else if (e == "connect") {
        disconnect();
        cancel_ = false;
        reqFps_ = data.value("fps", reqFps_);
        reqQuality_ = data.value("quality", reqQuality_);
        reqCustomBps_ = data.value("customBps", reqCustomBps_);
        setState("joining", "正在加入远控房间…");
        auto room = api_->request(
            "POST", "/api/v1/room/join/by_device/" + percentEncode(data.at("deviceId").get<std::string>()),
            {{"force_join", false}});
        auto url = roomSignalUrl(room);
        if (!room.contains("token") || !room["token"].is_string() ||
            room["token"].get_ref<const std::string &>().empty())
            throw std::runtime_error("加入房间未返回信令凭据");
        socket_ = std::make_unique<SignalSocket>(cancel_);
        setState("signaling", "正在连接 UU 信令服务器…");
        socket_->open(url, room.at("token").get<std::string>());
        lastPacket_ = monotonicMs();
        deadline_ = monotonicMs() + 45000;
    } else if (e == "input") {
        input(data);
    } else if (e == "release") {
        release();
    } else if (e == "local") {
        emit("soac",
             soac(remote_, data.at("type").get<std::string>(), {{"sdp", data.at("sdp")}}, ice_, control_));
        diagnostic(data.value("type", "") == "answer" ? "soac-answer-sent" : "soac-offer-sent");
    } else if (e == "candidate") {
        emit("soac", soac(remote_, "candidate", data, ice_, control_));
        diagnostic("soac-candidate-sent");
    } else if (e == "ice-state" || e == "gathering-state" || e == "signaling-state") {
        const auto label = "rtc-" + e + "-" + std::to_string(data.get<int>());
        diagnostic(label.c_str());
        if (e == "ice-state" && data != int(rtc::PeerConnection::IceState::Closed))
            iceState_ = static_cast<rtc::PeerConnection::IceState>(data.get<int>());
        if (e == "ice-state" &&
            (data == int(rtc::PeerConnection::IceState::Connected) ||
             data == int(rtc::PeerConnection::IceState::Completed)) &&
            !snapshot().streaming)
            setState("connecting", "网络通道已就绪，正在协商加密和数据通道…");
        if (e == "ice-state" &&
            (data == int(rtc::PeerConnection::IceState::Connected) ||
             data == int(rtc::PeerConnection::IceState::Completed)) && peer_) {
            auto typeName = [](rtc::Candidate::Type type) {
                switch (type) {
                case rtc::Candidate::Type::Host:
                    return "host";
                case rtc::Candidate::Type::PeerReflexive:
                    return "prflx";
                case rtc::Candidate::Type::ServerReflexive:
                    return "srflx";
                case rtc::Candidate::Type::Relayed:
                    return "relay";
                default:
                    return "unknown";
                }
            };
            rtc::Candidate local, remote;
            if (peer_->getSelectedCandidatePair(&local, &remote)) {
                std::lock_guard lock(stateMutex_);
                state_.pairType = std::string(typeName(remote.type())) + "/" + typeName(local.type());
                diagnostic(("rtc-pair-" + state_.pairType).c_str());
            }
            // Delay the first feedback until the transport has had time to settle.
            lastFeedback_ = monotonicMs();
        }
    } else if (e == "peer-state") {
        auto s = data.get<int>();
        diagnostic(("rtc-peer-state-" + std::to_string(s)).c_str());
        if (s == int(rtc::PeerConnection::State::Connected)) {
            deadline_ = 0;
            rpcSent_ = false;
            setState("connected", "已连接 · 左摇杆移动鼠标 · ZR/ZL 点击 · L3 显隐HUD · - 断开");
        } else if (s == int(rtc::PeerConnection::State::Failed) ||
                   s == int(rtc::PeerConnection::State::Disconnected) ||
                   s == int(rtc::PeerConnection::State::Closed)) {
            if (snapshot().streaming)
                throw std::runtime_error("音视频连接已中断，请重新连接");
            if (iceState_ == rtc::PeerConnection::IceState::Connected ||
                iceState_ == rtc::PeerConnection::IceState::Completed)
                throw std::runtime_error("加密或数据通道建立失败，详细阶段见 connection.log");
            throw std::runtime_error("ICE 网络连接失败，详细阶段见 connection.log");
        }
    } else if (e == "async-error")
        throw std::runtime_error(data.get<std::string>());
}
void Client::pollQr() {
    if (monotonicMs() > qrExpires_) {
        qr_ = nullptr;
        {
            std::lock_guard lock(stateMutex_);
            state_.qrUrl.clear();
        }
        setState("idle", "二维码已过期，按 A 重新生成");
        return;
    }
    Json data = qr_;
    data["login_status"] = qrStatus_;
    auto result = api_->request("POST", "/api/v1/qrcode/login/status", data, 65000);
    qrStatus_ = result.value("login_status", 1);
    qrNext_ = monotonicMs() + 1000;
    if (qrStatus_ == 4) {
        auto login = api_->request("POST", "/api/v1/login/by_qrcode", qr_);
        completeLogin(login);
    } else if (qrStatus_ == 3) {
        qr_ = nullptr;
        setState("idle", "手机已取消登录，按 A 重试");
    } else if (qrStatus_ == 2)
        setState("login", "扫码成功，请在手机上确认登录");
}
void Client::completeLogin(const Json &response) {
    if (cancel_)
        throw std::runtime_error("Canceled");
    auto account = loginAccount(response);
    api_->login(response);
    if (!sessionPath_.empty()) {
        try {
            std::ofstream f(sessionPath_, std::ios::binary | std::ios::trunc);
            f << Json{{"userId", account.userId},
                      {"token", account.token},
                      {"nickname", account.nickname}}
                     .dump();
        } catch (...) {
        }
    }
    qr_ = nullptr;
    {
        std::lock_guard lock(stateMutex_);
        state_.qrUrl.clear();
        state_.nickname = account.nickname;
        state_.loginMode = "qr";
    }
    setState("loading", "登录成功，正在同步设备…");
    refresh();
}
void Client::tryRestore() {
    auto saved = loadSession(sessionPath_);
    if (saved.is_null())
        return;
    setState("loading", "正在恢复登录状态…");
    try {
        api_->initialize();
        api_->restore(saved.at("userId").get<std::string>(), saved.at("token").get<std::string>());
        {
            std::lock_guard lock(stateMutex_);
            state_.nickname = saved.value("nickname", std::string{});
        }
        refresh();
    } catch (const std::exception &) {
        if (api_->authenticated()) {
            // 网络等非凭证问题：保留会话文件，用户可按 Y 重试或按 X 退出。
            setState("idle", "登录恢复失败，请检查网络后按 Y 重试");
            return;
        }
        std::remove(sessionPath_.c_str());
        setState("idle", "登录状态已过期，请按 A 重新扫码登录");
    }
}
void Client::refresh() {
    auto response = api_->request("GET", "/api/v1/device/list");
    Json devices = Json::array();
    std::set<std::string> seen;
    for (auto name : {"my_binded_devices", "others_shared_devices"})
        if (response.contains(name) && response[name].is_array())
            for (auto d : response[name]) {
                auto id = d.value("device_id", std::string{});
                if (id.empty() || id == api_->device() || !seen.insert(id).second)
                    continue;
                devices.push_back(std::move(d));
            }
    bool empty = devices.empty();
    {
        std::lock_guard lock(stateMutex_);
        state_.devices = std::move(devices);
    }
    setState("idle", empty ? "暂无电脑，请在原版 UU 电脑端登录同一账号"
                           : "选择电脑，按 A 连接 · Y 刷新 · X 退出账号");
}
void Client::emit(const std::string &name, const Json &data, const std::string &ack,
                  const Bytes &attachment) {
    if (!socket_)
        return;
    Json args = Json::array({name});
    if (!data.is_null())
        args.push_back(data);
    std::string id;
    if (!ack.empty()) {
        int n = nextAck_++;
        acks_[n] = ack;
        id = std::to_string(n);
    }
    socket_->send((attachment.empty() ? "42" : "451-") + id + args.dump());
    if (!attachment.empty())
        socket_->sendBinary(attachment);
}
void Client::packet(const WsMessage &message) {
    lastPacket_ = monotonicMs();
    if (message.opcode == 2) {
        if (remainingAttachments_ <= 0)
            throw std::runtime_error("Unexpected signaling attachment");
        if (--remainingAttachments_ == 0)
            dispatch(pending_);
        return;
    }
    std::string text(message.data.begin(), message.data.end());
    if (text.empty())
        return;
    if (text[0] == '0') {
        auto hello = Json::parse(text.substr(1));
        heartbeatTimeout_ = hello.value("pingInterval", 25000) + hello.value("pingTimeout", 20000) + 5000;
        socket_->send("40");
        return;
    }
    if (text[0] == '2') {
        socket_->send("3" + text.substr(1));
        return;
    }
    if (text[0] == '1')
        throw std::runtime_error("UU 已关闭连接");
    if (text[0] != '4')
        return;
    if (remainingAttachments_)
        throw std::runtime_error("Interleaved signaling attachment");
    auto p = parseSocketIo(text);
    if (p.attachments) {
        pending_ = p;
        remainingAttachments_ = p.attachments;
    } else
        dispatch(p);
}
void Client::dispatch(const SioPacket &p) {
    if (p.type == 0) {
        emit("room_info", nullptr, "room");
        return;
    }
    if (p.type == 1 || p.type == 4)
        throw std::runtime_error("UU 信令拒绝连接");
    if (p.type == 3 || p.type == 6) {
        auto it = acks_.find(p.id);
        if (it == acks_.end())
            return;
        auto event = it->second;
        acks_.erase(it);
        auto value = ackValue(p.data, event);
        if (event == "room") {
            auto publisher = value.at("publisher");
            remote_ = publisher.is_object() ? publisher.value("client_id", publisher.value("id", Json{}))
                                            : publisher;
            if ((!remote_.is_string() && !remote_.is_number()) || remote_ == "")
                throw std::runtime_error("房间内没有在线电脑");
            control_ = uuid();
            {
                // 记录本次会话请求的画质参数，便于日志与服务器行为对齐。
                char reqLine[64];
                std::snprintf(reqLine, sizeof reqLine, "req-q%d-f%d-bps%llu", reqQuality_, reqFps_,
                              static_cast<unsigned long long>(reqCustomBps_));
                diagnostic(reqLine);
            }
            auto options = connectOptions(1280, 720, reqFps_, reqQuality_, reqCustomBps_,
                                          api_->device());
            options.insert(options.begin(), 4);
            setState("negotiating", "正在申请远控权限…");
            // Preserve room ICE configuration if control does not repeat it.
            ice_ = value.value("iceServers", Json::array());
            // 控制初始化必须携带 device_capability（对齐官方客户端 control_header）：
            // 空的 streamer_data 会让服务器按"未知最低版本客户端"处理（~2.5M 帽+砍帧）。
            Json display = Json::object();
            display["id"] = 0;
            display["fps"] = 144;
            display["type"] = 1;
            display["hdr"] = 0;
            Json codec = Json::object();
            codec["video_codec"] = 1;
            codec["chroma_sampling"] = 1;
            codec["codec_impl"] = 5;
            codec["bit_depth"] = 8;
            codec["width"] = 1920;
            codec["height"] = 1080;
            Json devcap = Json::object();
            devcap["display_info"] = Json::array({display});
            devcap["ice_id"] = "";
            devcap["video_codec_capability"] = Json::array({codec});
            Json capability = Json::object();
            capability["control_id"] = control_;
            capability["device_capability"] = devcap;
            emit("control",
                 {{"app_data", {{"_placeholder", true}, {"num", 0}}},
                  {"streamer_data", capability.dump()},
                  {"app_control_id", control_}},
                 "control", options);
        } else if (event == "control")
            beginPeer(value);
    } else if (p.type == 2 || p.type == 5) {
        if (!p.data.is_array() || p.data.empty() || !p.data[0].is_string())
            throw std::runtime_error("Invalid signaling event");
        auto event = p.data[0].get<std::string>();
        if (event == "soac" && p.data.size() > 1) {
            if (peer_)
                handleSoac(p.data[1]);
            else if (earlySoac_.size() < 128)
                earlySoac_.push_back(p.data[1]);
        } else if (event == "released" || event == "left")
            throw std::runtime_error("远控权限已释放或电脑已退出房间");
        if (p.id >= 0)
            socket_->send("43" + std::to_string(p.id) + "[]");
    }
}
void Client::beginPeer(const Json &result) {
    auto servers = result.value("iceServers", ice_);
    remote_ = result.value("client_id", remote_);
    ice_ = result.at("ice_id");
    rtc::Configuration config;
    config.disableAutoNegotiation = true;
    if (result.value("force_relay", false))
        config.iceTransportPolicy = rtc::TransportPolicy::Relay;
    for (const auto &s : servers) {
        auto urls = s.value("urls", s.value("url", Json{}));
        if (urls.is_string())
            urls = Json::array({urls});
        if (!urls.is_array())
            continue;
        for (const auto &u : urls) {
            rtc::IceServer server(u.get<std::string>());
            server.username = s.value("username", std::string{});
            server.password = s.value("credential", s.value("password", std::string{}));
            config.iceServers.push_back(std::move(server));
        }
    }
    setState("rtc-init", "正在初始化原生串流…");
    diagnostic("rtc-runtime");
    rtc::Preload();
    diagnostic("rtc-peer");
    peer_ = std::make_shared<rtc::PeerConnection>(config);
    auto weak = std::weak_ptr<Inbox>(inbox_);
    auto gen = generation_;
    auto post = [weak, gen](std::string type, Json data) {
        if (auto box = weak.lock()) {
            std::lock_guard lock(box->mutex);
            if (box->events.size() < 2048)
                box->events.push_back({std::move(type), std::move(data), gen});
            box->cv.notify_one();
        }
    };
    peer_->onLocalDescription([post](rtc::Description desc) {
        post("local", {{"type", desc.typeString()}, {"sdp", std::string(desc)}});
    });
    peer_->onLocalCandidate([post](rtc::Candidate c) {
        post("candidate", {{"candidate", std::string(c)}, {"sdpMid", c.mid()}, {"sdpMLineIndex", 0}});
    });
    peer_->onStateChange([post](rtc::PeerConnection::State s) { post("peer-state", int(s)); });
    peer_->onIceStateChange([post](rtc::PeerConnection::IceState s) { post("ice-state", int(s)); });
    peer_->onGatheringStateChange(
        [post](rtc::PeerConnection::GatheringState s) { post("gathering-state", int(s)); });
    peer_->onSignalingStateChange(
        [post](rtc::PeerConnection::SignalingState s) { post("signaling-state", int(s)); });
    rtc::Description::Video v("0", rtc::Description::Direction::RecvOnly);
    diagnostic("rtc-video");
    // H.265 优先声明（同码率画质高 30%~50%，Switch NVDEC 可硬解）；同时保留 H.264。
    v.addH265Codec(97);
    v.addH264Codec(96, "level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42e01f");
    v.addAttribute("rtcp-fb:96 nack");     // 声明支持重传请求：服务器才保留重传历史
    v.addAttribute("rtcp-fb:96 nack pli");
    v.addAttribute("rtcp-fb:97 nack");
    v.addAttribute("rtcp-fb:97 nack pli");
    v.setBitrate(20000);
    // 旧 draft URI 才能被服务器的老版 libwebrtc 识别（RFC 8888 的 URN 会被忽略）。
    v.addExtMap(rtc::Description::Media::ExtMap(
        1, "http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01"));
    video_ = peer_->addTrack(v);
    media_->setOverflowCallback([weak = std::weak_ptr<rtc::Track>(video_)] {
        if (auto track = weak.lock()) {
            try {
                track->requestKeyframe();
            } catch (...) {
            }
        }
    });
    auto tcc = std::make_shared<TransportCcHandler>();
    videoPt_ = -1;
    tcc->setPayloadTypeSink(&videoPt_);    tcc->setOnGap([weak = std::weak_ptr<rtc::Track>(video_),
                   weakMedia = std::weak_ptr<Media>(media_)] {
        if (auto m = weakMedia.lock())
            m->invalidate(); // 冻结坏画面 + 清队 + 请求 IDR（内部节流）
        else if (auto track = weak.lock()) {
            try {
                track->requestKeyframe();
            } catch (...) {
            }
        }
    });
    auto rtcpSession = std::make_shared<rtc::RtcpReceivingSession>();
    // 链序：depacketizer → tcc → keyframe → rtcpSession。incomingChain 先递归后自身，
    // 所以 tcc（后位）先于 depacketizer 看到带头的原始 RTP 包；
    // requestBitrate 从链头转发，tcc 发 transport-cc 反馈，rtcpSession 发 REMB。
    auto keyframe = std::make_shared<KeyframeRequestHandler>();
    // answer 里回显了 H265/90000 就切 H.265 解包器；否则保持 H.264。
    attachVideo_ = [this, tcc, keyframe, rtcpSession](bool hevc) {
        std::shared_ptr<rtc::MediaHandler> depacketizer;
        if (hevc)
            depacketizer =
                std::make_shared<rtc::H265RtpDepacketizer>(rtc::NalUnit::Separator::StartSequence);
        else
            depacketizer =
                std::make_shared<rtc::H264RtpDepacketizer>(rtc::NalUnit::Separator::StartSequence);
        depacketizer->setNext(tcc);
        tcc->setNext(keyframe);
        keyframe->setNext(rtcpSession);
        rtcpSession->setNext(nullptr);
        video_->setMediaHandler(depacketizer);
    };
    attachVideo_(false);
    videoHevc_ = false;
    media_->setCodec(false);
    std::weak_ptr<Media> media = media_;
    auto mediaGeneration = media_->generation();
    video_->onFrame([media, mediaGeneration](rtc::binary data, rtc::FrameInfo) {
        if (auto m = media.lock())
            m->video(reinterpret_cast<const uint8_t *>(data.data()), data.size(), mediaGeneration);
    });
    rtc::Description::Audio a("1", rtc::Description::Direction::RecvOnly);
    diagnostic("rtc-audio");
    a.addOpusCodec(111);
    audio_ = peer_->addTrack(a);
    auto opus = std::make_shared<rtc::OpusRtpDepacketizer>();
    opus->addToChain(std::make_shared<rtc::RtcpReceivingSession>());
    audio_->setMediaHandler(opus);
    audio_->onFrame([media, mediaGeneration](rtc::binary data, rtc::FrameInfo) {
        if (auto m = media.lock())
            m->audio(reinterpret_cast<const uint8_t *>(data.data()), data.size(), mediaGeneration);
    });
    diagnostic("rtc-channels");
    for (const auto *label : {"CONTROL_DATA_CHANNEL", "TEXT_DATA_CHANNEL", "STREAMER_DATA_CHANNEL",
                              "FILE_DATA_CHANNEL", "BINARY_DATA_CHANNEL"}) {
        auto channel = peer_->createDataChannel(label);
        if (std::strcmp(label, "CONTROL_DATA_CHANNEL") == 0)
            channel->onMessage([this](rtc::message_variant message) {
                if (auto *data = std::get_if<rtc::binary>(&message))
                    handleControlData(reinterpret_cast<const uint8_t *>(data->data()), data->size());
            });
        channels_[label] = std::move(channel);
    }
    setState("connecting", "正在建立音视频通道…");
    diagnostic("rtc-offer");
    peer_->setLocalDescription();
    for (const auto &e : earlySoac_)
        handleSoac(e);
    earlySoac_.clear();
}
void Client::handleSoac(const Json &packet) {
    if (!packet.contains("data") || !packet["data"].is_object())
        return;
    const auto &d = packet["data"];
    if (d.contains("ice_id")) {
        auto idString = [](const Json &id) { return id.is_string() ? id.get<std::string>() : id.dump(); };
        if (idString(d["ice_id"]) != idString(ice_))
            return;
    }
    const auto type = d.value("type", std::string{});
    if (type == "answer")
        diagnostic("soac-answer-received");
    else if (type == "offer" || type == "restart_ice")
        diagnostic("soac-offer-received");
    if (type == "answer") {
        const auto sdp = d.value("sdp", std::string{});
        if (!sdp.empty()) {
            dumpAnswerSdp(sdp);
            diagnostic(sdp.find("transport-wide-cc") == std::string::npos ? "sdp-twcc-off"
                                                                          : "sdp-twcc-on");
            diagnostic(sdp.find("nack") == std::string::npos ? "sdp-nack-off" : "sdp-nack-on");
        }
    }
    const auto result = applyRemoteSignal(*peer_, d, pendingCandidates_);
    if (result == RemoteSignal::Closed)
        diagnostic("soac-after-transport-close");
    else if (result == RemoteSignal::CandidateQueued)
        diagnostic("soac-candidate-queued");
    else if (result == RemoteSignal::CandidateApplied)
        diagnostic("soac-candidate-applied");
    else if (result == RemoteSignal::Description)
        diagnostic("soac-description-applied");
}
void Client::input(const Json &data) {
    auto it = channels_.find("CONTROL_DATA_CHANNEL");
    if (it == channels_.end() || !it->second->isOpen())
        return;
    const auto action = data.value("action", std::string{});
    if (it->second->bufferedAmount() > 64 * 1024 &&
        (action == "mouse_move_absolute" || action == "mouse_scroll"))
        return;
    auto s = data.dump();
    try {
        it->second->send(reinterpret_cast<const std::byte *>(s.data()), s.size());
    } catch (...) {
        return;
    }
    if (action == "kbd_press")
        heldKeys_.insert(data.at("key").get<int>());
    else if (action == "kbd_release")
        heldKeys_.erase(data.at("key").get<int>());
    else if (action == "mouse_press")
        heldButtons_.insert(data.at("button").get<int>());
    else if (action == "mouse_release")
        heldButtons_.erase(data.at("button").get<int>());
}
void Client::release() {
    auto keys = heldKeys_, buttons = heldButtons_;
    for (int k : keys)
        input({{"action", "kbd_release"}, {"key", k}});
    for (int b : buttons)
        input({{"action", "mouse_release"}, {"button", b}});
    heldKeys_.clear();
    heldButtons_.clear();
}
void Client::handleControlData(const uint8_t *data, size_t size) {
    Bytes raw(data, data + size);
    auto fields = decodeWire(raw);
    const Bytes *action = nullptr, *sync = nullptr, *response = nullptr, *qos = nullptr;
    uint64_t seq = 0;
    for (auto &f : fields) {
        if (f.number == 1 && !f.isBytes)
            seq = f.varint;
        else if (f.number == 3 && f.isBytes)
            action = &f.bytes;
        else if (f.number == 14 && f.isBytes)
            qos = &f.bytes;
        else if (f.number == 22 && f.isBytes)
            response = &f.bytes;
        else if (f.number == 25 && f.isBytes)
            sync = &f.bytes;
    }
    if (action) {
        auto inner = decodeWire(*action);
        uint64_t act = 0;
        for (auto &f : inner)
            if (f.number == 1 && !f.isBytes)
                act = f.varint;
        if (act == 0) { // 被控端 echo 请求：入队，由主循环应答
                        // （回调内 send 会与收包处理争锁导致整进程冻结）
            pendingEchoSeq_ = seq;
        }
        return;
    }
    if (sync) { // Message 25：服务器实际生效的画质/帧率/分辨率
        auto inner = decodeWire(*sync);
        uint64_t quality = 0, fpsEnum = 0, w = 0, h = 0;
        for (auto &f : inner) {
            if (f.isBytes)
                continue;
            if (f.number == 1)
                fpsEnum = f.varint;
            else if (f.number == 2)
                quality = f.varint;
            else if (f.number == 5)
                w = f.varint;
            else if (f.number == 6)
                h = f.varint;
        }
        if (!appliedLogged_) {
            appliedLogged_ = true;
            diagnostic(("applied-q" + std::to_string(quality) + "-fps" + std::to_string(fpsEnum) +
                        "-" + std::to_string(w) + "x" + std::to_string(h))
                           .c_str());
        }
        return;
    }
    if (response) { // Message 22：RPC 应答（含错误码）
        auto inner = decodeWire(*response);
        for (auto &f : inner) {
            if (f.number != 2 || !f.isBytes)
                continue;
            auto resp = decodeWire(f.bytes);
            for (auto &r : resp) {
                if (r.number != 1 || !r.isBytes)
                    continue;
                auto errors = decodeWire(r.bytes);
                for (auto &e : errors) {
                    if (e.number == 1 && !e.isBytes && e.varint != 0) {
                        diagnostic("rpc-resp-error");
                        return;
                    }
                }
            }
        }
        diagnostic("rpc-resp-ok");
        return;
    }
    if (qos && !qosLogged_) {
        qosLogged_ = true;
        diagnostic("qos-stats-recv");
    }
}
void Client::sendRpcSettings() {
    if (rpcSent_)
        return;
    auto it = channels_.find("CONTROL_DATA_CHANNEL");
    if (it == channels_.end() || !it->second->isOpen())
        return;
    try {
        uint64_t seq = nextSeq_++;
        {
            auto packet = rpcCaptureSettings(seq, seq, reqQuality_, reqFps_, reqCustomBps_);
            it->second->send(reinterpret_cast<const std::byte *>(packet.data()), packet.size());
        }
        seq = nextSeq_++;
        {
            auto packet = rpcQuality(seq, seq, reqQuality_);
            it->second->send(reinterpret_cast<const std::byte *>(packet.data()), packet.size());
        }
        seq = nextSeq_++;
        {
            auto packet = rpcFps(seq, seq, reqFps_);
            it->second->send(reinterpret_cast<const std::byte *>(packet.data()), packet.size());
        }
        rpcSent_ = true;
        diagnostic("rpc-settings-sent");
    } catch (...) {
        diagnostic("rpc-settings-failed");
    }
}
void Client::disconnect() {
    diagnostic("disconnect");
    release();
    ++generation_;
    iceState_ = rtc::PeerConnection::IceState::New;
    deadline_ = 0;
    lastFeedback_ = 0;
    lastEchoMs_ = 0;
    rpcSent_ = false;
    appliedLogged_ = false;
    qosLogged_ = false;
    echoReplied_ = false;
    pendingEchoSeq_ = 0;
    qr_ = nullptr;
    earlySoac_.clear();
    pendingCandidates_.clear();
    acks_.clear();
    remainingAttachments_ = 0;
    nextAck_ = 1;
    {
        // Flip the UI back to the menu before any potentially slow teardown.
        std::lock_guard lock(stateMutex_);
        state_.qrUrl.clear();
        state_.pairType.clear();
        state_.streaming = false;
    }
    // Closing channels and the peer can block on transport shutdown; run it on
    // a background task so the worker thread stays responsive.
    auto channels = std::move(channels_);
    auto video = std::move(video_);
    auto audio = std::move(audio_);
    auto peer = std::move(peer_);
    backgroundTask([channels = std::move(channels), video = std::move(video),
                    audio = std::move(audio), peer = std::move(peer)]() mutable {
        for (auto &channel : channels)
            channel.second->close();
        video.reset();
        audio.reset();
        if (peer) {
            peer->resetCallbacks();
            peer->close();
        }
    });
    socket_.reset();
    media_->reset();
}
} // namespace uu
