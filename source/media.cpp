#include "media.hpp"
#include "diagnostics.hpp"
#include <opus.h>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libswscale/swscale.h>
}
#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace uu {
namespace {
// 一帧可能由多个 NAL 拼接（SPS+PPS+IDR slice），必须扫描整帧而不是只看第一个。
struct NalSummary {
    bool idr = false;
    bool slice = false;
};
NalSummary scanNals(const uint8_t *data, size_t size, bool hevc) {
    NalSummary out;
    auto inspect = [&out, hevc](const uint8_t *header) {
        if (hevc) {
            // HEVC: nal_unit_type = (byte >> 1) & 0x3F；16~21 = BLA/IDR/CRA（关键帧）
            const uint8_t type = uint8_t((*header >> 1) & 0x3F);
            if (type >= 16 && type <= 21)
                out.idr = true;
        } else {
            const uint8_t type = *header & 0x1F;
            if (type == 5)
                out.idr = true;
            else if (type >= 1 && type <= 4)
                out.slice = true;
        }
    };
    for (size_t i = 0; i + 3 < size;) {
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
            inspect(&data[i + 3]);
            i += 4;
        } else if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 0 && i + 4 < size &&
                   data[i + 3] == 1) {
            inspect(&data[i + 4]);
            i += 5;
        } else {
            ++i;
        }
    }
    return out;
}
struct DecoderResources {
    AVCodecContext *context = nullptr;
    AVFrame *frame = nullptr, *cpu = nullptr;
    AVPacket *packet = nullptr;
    SwsContext *scaler = nullptr;
    ~DecoderResources() {
        sws_freeContext(scaler);
        av_packet_free(&packet);
        av_frame_free(&cpu);
        av_frame_free(&frame);
        avcodec_free_context(&context);
    }
};
} // namespace
struct Media::Impl {
    std::mutex mutex, audioMutex;
    std::condition_variable cv;
    std::deque<Bytes> queue;
    // 已解码待显示的帧。网络突发时解码会一次产出多帧，直接显示最新帧会造成
    // "一次跳几帧 + 随后卡住"的抖动；用小缓冲按显示节奏逐帧释放更顺。
    std::deque<std::shared_ptr<Picture>> ready;
    std::shared_ptr<Picture> shown;
    int64_t shownAtMs = 0;
    static constexpr size_t kReadyDepth = 2; // 缓冲 2 帧（≈33ms 延迟上限）
    bool stop = false, clear = false;
    std::atomic<bool> waitingKeyframe{false};
    std::atomic<bool> gapPending{false};
    std::atomic<int64_t> waitingSince{0};
    std::atomic<bool> loggedFrame{false};
    std::atomic<int> codecId{0}; // 0 = H.264, 1 = H.265
    int64_t lastAudioClearMs = 0;
    std::function<void()> onOverflow;
    int64_t lastOverflowPli = 0;
    std::thread thread;
    SDL_AudioDeviceID audioDevice = 0;
    OpusDecoder *opus = nullptr;
    std::string error;
    uint64_t serial = 0;
    std::atomic<uint64_t> generation{1};
    std::atomic<bool> decoderFailed{false};
    enum class AudioStatus { Ready, Unavailable, QueueFailed };
    std::atomic<AudioStatus> audioStatus{AudioStatus::Ready};
    Impl() {
        try {
            SDL_AudioSpec spec{};
            spec.freq = 48000;
            spec.format = AUDIO_S16SYS;
            spec.channels = 2;
            spec.samples = 960;
            audioDevice = SDL_OpenAudioDevice(nullptr, 0, &spec, nullptr, 0);
            if (!audioDevice)
                audioStatus = AudioStatus::Unavailable;
            int code = 0;
            opus = opus_decoder_create(48000, 2, &code);
            if (code != OPUS_OK)
                throw std::runtime_error("Opus decoder initialization failed");
            if (audioDevice)
                SDL_PauseAudioDevice(audioDevice, 0);
            thread = std::thread([this] {
                try {
                    decode();
                } catch (...) {
                    decoderFailed = true;
                    diagnosticException("video-decode-exception", std::current_exception());
                }
            });
        } catch (...) {
            if (audioDevice)
                SDL_CloseAudioDevice(audioDevice);
            opus_decoder_destroy(opus);
            throw;
        }
    }
    ~Impl() {
        {
            std::lock_guard lock(mutex);
            stop = true;
        }
        cv.notify_one();
        thread.join();
        if (audioDevice)
            SDL_CloseAudioDevice(audioDevice);
        opus_decoder_destroy(opus);
    }
    static AVPixelFormat chooseFormat(AVCodecContext *context, const AVPixelFormat *formats) {
#ifdef __SWITCH__
        for (auto p = formats; *p != AV_PIX_FMT_NONE; ++p)
            if (context->hw_device_ctx && *p == AV_PIX_FMT_NVTEGRA)
                return *p;
#else
        (void)context;
#endif
        for (auto p = formats; *p != AV_PIX_FMT_NONE; ++p)
            if (*p == AV_PIX_FMT_YUV420P || *p == AV_PIX_FMT_NV12)
                return *p;
        return formats[0];
    }
    void decode() {
        DecoderResources resources;
        auto *&ctx = resources.context;
        auto *frame = resources.frame = av_frame_alloc();
        auto *cpu = resources.cpu = av_frame_alloc();
        auto *packet = resources.packet = av_packet_alloc();
        auto *&sws = resources.scaler;
        if (!frame || !cpu || !packet) {
            std::lock_guard lock(mutex);
            error = "视频解码器内存不足";
            decoderFailed = true;
            return;
        }
        int openedCodec = -1;
        // 协商结果可能是 H.264 或 H.265：按 codecId 懒创建/切换解码器。
        auto openDecoder = [&](int id) {
            if (ctx)
                avcodec_free_context(&ctx);
            const auto *codec =
                avcodec_find_decoder(id == 1 ? AV_CODEC_ID_HEVC : AV_CODEC_ID_H264);
            if (!codec) {
                std::lock_guard lock(mutex);
                error = "设备不支持该视频编码";
                decoderFailed = true;
                return false;
            }
            ctx = avcodec_alloc_context3(codec);
            if (!ctx) {
                std::lock_guard lock(mutex);
                error = "视频解码器内存不足";
                decoderFailed = true;
                return false;
            }
            ctx->thread_count = 2;
            ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
#ifdef __SWITCH__
            ctx->get_format = chooseFormat;
            if (av_hwdevice_ctx_create(&ctx->hw_device_ctx, AV_HWDEVICE_TYPE_NVTEGRA, nullptr,
                                       nullptr, 0) < 0) {
                std::lock_guard lock(mutex);
                error = "NVDEC 不可用，使用软件解码";
            }
#endif
            if (avcodec_open2(ctx, codec, nullptr) < 0) {
                std::lock_guard lock(mutex);
                error = id == 1 ? "无法初始化 H.265 解码器" : "无法初始化 H.264 解码器";
                decoderFailed = true;
                avcodec_free_context(&ctx);
                return false;
            }
            diagnostic(id == 1 ? "vid-decoder-h265" : "vid-decoder-h264");
            openedCodec = id;
            return true;
        };
        if (!openDecoder(codecId.load()))
            return;
        while (true) {
            const int wantCodec = codecId.load();
            if (wantCodec != openedCodec && !openDecoder(wantCodec))
                break;
            Bytes data;
            uint64_t currentGeneration;
            {
                std::unique_lock lock(mutex);
                cv.wait(lock, [&] { return stop || clear || !queue.empty(); });
                if (stop)
                    break;
                if (clear) {
                    avcodec_flush_buffers(ctx);
                    clear = false;
                }
                if (queue.empty())
                    continue;
                data = std::move(queue.front());
                queue.pop_front();
                currentGeneration = generation.load();
            }
            av_packet_unref(packet);
            if (av_new_packet(packet, int(data.size())) < 0)
                continue;
            std::copy(data.begin(), data.end(), packet->data);
            auto drain = [&] {
                while (avcodec_receive_frame(ctx, frame) == 0) {
                    AVFrame *source = frame;
#ifdef __SWITCH__
                    if (frame->format == AV_PIX_FMT_NVTEGRA) {
                        av_frame_unref(cpu);
                        if (av_hwframe_transfer_data(cpu, frame, 0) < 0) {
                            av_frame_unref(frame);
                            continue;
                        }
                        source = cpu;
                    }
#endif
                    if (source->width <= 0 || source->height <= 0 || source->width > 4096 ||
                        source->height > 2160) {
                        av_frame_unref(frame);
                        continue;
                    }
                    Picture p;
                    p.width = source->width;
                    p.height = source->height;
                    if (!loggedFrame.exchange(true)) {
                        char line[48];
                        std::snprintf(line, sizeof line, "vid-frame-%dx%d", p.width, p.height);
                        diagnostic(line);
                    }
                    // 转 RGBA：SDL（Switch 端口）不支持 NV12 纹理，只能 CPU 转换。
                    p.pixels.resize(size_t(p.width) * p.height * 4);
                    sws = sws_getCachedContext(sws, p.width, p.height, AVPixelFormat(source->format),
                                               p.width, p.height, AV_PIX_FMT_RGBA, SWS_FAST_BILINEAR,
                                               nullptr, nullptr, nullptr);
                    if (sws) {
                        uint8_t *dst[] = {p.pixels.data()};
                        int stride[] = {p.width * 4};
                        sws_scale(sws, source->data, source->linesize, 0, p.height, dst, stride);
                    }
                    if (!sws) {
                        av_frame_unref(frame);
                        continue;
                    }
                    {
                        std::lock_guard lock(mutex);
                        if (currentGeneration == generation.load()) {
                            p.serial = ++serial;
                            ready.push_back(std::make_shared<Picture>(std::move(p)));
                            while (ready.size() > kReadyDepth)
                                ready.pop_front(); // 超出缓冲深度丢最旧帧，限制延迟
                        }
                    }
                    av_frame_unref(frame);
                }
            };
            int result = avcodec_send_packet(ctx, packet);
            if (result == AVERROR(EAGAIN)) {
                drain();
                result = avcodec_send_packet(ctx, packet);
            }
            if (result >= 0)
                drain();
        }
    }
};
Media::Media() : impl_(std::make_unique<Impl>()) {}
Media::~Media() = default;
void Media::video(const uint8_t *data, size_t size, uint64_t generation) {
    if (!size || size > 2 * 1024 * 1024 || impl_->decoderFailed.load())
        return;
    // NAL 类型（兼容有无 Annex-B 起始码）：等待关键帧期间丢弃普通 slice
    // （类型 1-4），放行 IDR/SPS/PPS/SEI——链断后从关键帧干净恢复。
    {
        const auto nals = scanNals(data, size, impl_->codecId.load() == 1);
        if (nals.idr) {
            if (impl_->gapPending.exchange(false)) {
                char line[48];
                std::snprintf(line, sizeof line, "vid-idr-gap-%lld",
                              static_cast<long long>(monotonicMs() - impl_->waitingSince.load()));
                diagnostic(line);
            } else {
                diagnostic("vid-idr");
            }
            impl_->waitingKeyframe = false;
        }
        if (impl_->waitingKeyframe.load()) {
            // 服务器无视 PLI 时不能无限冻结：1.5 秒仍无关键帧就放弃，恢复播放。
            if (monotonicMs() - impl_->waitingSince.load() > 1500) {
                impl_->waitingKeyframe = false;
                diagnostic("vid-freeze-timeout");
            } else {
                return;
            }
        }
    }
    {
        std::lock_guard lock(impl_->mutex);
        if (generation != impl_->generation.load())
            return;
        if (impl_->queue.size() >= 8) {
            impl_->queue.clear();
            impl_->clear = true;
            // 解码跟不上时清空队列会破坏 P 帧参考链——等待下一个 IDR 期间
            // 丢弃普通 slice，并立即请求关键帧（把 ~8 秒花屏压缩为瞬时定格）。
            impl_->waitingKeyframe = true;
            impl_->waitingSince = monotonicMs();
            diagnostic("vid-ovf");
            if (impl_->onOverflow && monotonicMs() - impl_->lastOverflowPli > 800) {
                impl_->lastOverflowPli = monotonicMs();
                impl_->onOverflow();
            }
        }
        impl_->queue.emplace_back(data, data + size);
    }
    impl_->cv.notify_one();
}
void Media::invalidate() {
    bool request = false;
    {
        std::lock_guard lock(impl_->mutex);
        // 清掉已解码但还没显示的帧：其中可能包含坏帧，冻结在最后一张好画面
        // 等 IDR 更干净（IDR 通常 10ms 内就到）。
        impl_->ready.clear();
        impl_->waitingKeyframe = true;
        impl_->waitingSince = monotonicMs();
        impl_->gapPending = true;
        if (impl_->onOverflow && monotonicMs() - impl_->lastOverflowPli > 800) {
            impl_->lastOverflowPli = monotonicMs();
            request = true;
        }
    }
    diagnostic("vid-gap-freeze");
    if (request)
        impl_->onOverflow();
}
void Media::setCodec(bool hevc) {
    {
        std::lock_guard lock(impl_->mutex);
        impl_->queue.clear();
        impl_->ready.clear();
        impl_->shown.reset();
        impl_->shownAtMs = 0;
        impl_->clear = true;
        impl_->waitingKeyframe = true;
        impl_->waitingSince = monotonicMs();
    }
    impl_->codecId = hevc ? 1 : 0;
    impl_->cv.notify_one();
    diagnostic(hevc ? "codec-h265" : "codec-h264");
}
void Media::setOverflowCallback(std::function<void()> callback) {
    impl_->onOverflow = std::move(callback);
}
void Media::audio(const uint8_t *data, size_t size, uint64_t generation) {
    if (!size || size > 4096)
        return;
    std::lock_guard lock(impl_->audioMutex);
    if (generation != impl_->generation.load())
        return;
    std::array<int16_t, 5760 * 2> pcm{};
    int samples = opus_decode(impl_->opus, data, int(size), pcm.data(), 5760, 0);
    if (samples > 0 && impl_->audioDevice) {
        if (SDL_GetQueuedAudioSize(impl_->audioDevice) > 48000 * 4 / 5) {
            SDL_ClearQueuedAudio(impl_->audioDevice);
            if (monotonicMs() - impl_->lastAudioClearMs > 5000) {
                impl_->lastAudioClearMs = monotonicMs();
                diagnostic("aud-clear"); // 音频队列积压被清空：可能听到短暂断音
            }
        }
        impl_->audioStatus = SDL_QueueAudio(impl_->audioDevice, pcm.data(), samples * 4) == 0
                                 ? Impl::AudioStatus::Ready
                                 : Impl::AudioStatus::QueueFailed;
    }
}
std::shared_ptr<const Picture> Media::picture(uint64_t after) {
    (void)after;
    std::lock_guard lock(impl_->mutex);
    const int64_t now = monotonicMs();
    // 至少隔一帧间隔（60Hz ≈ 16ms）才释放下一帧：突发到达的多帧会被摊平到
    // 后续的显示刷新里，画面节奏稳定；缓冲为空时继续显示上一帧。
    if (!impl_->ready.empty() && now - impl_->shownAtMs >= 12) {
        impl_->shown = std::move(impl_->ready.front());
        impl_->ready.pop_front();
        impl_->shownAtMs = now;
    }
    return impl_->shown;
}
void Media::reset() {
    {
        std::lock_guard lock(impl_->mutex);
        // The decoder reads the generation while removing queued frames under
        // this lock. Advancing it earlier could relabel an old queued frame.
        impl_->generation.fetch_add(1);
        impl_->queue.clear();
        impl_->ready.clear();
        impl_->shown.reset();
        impl_->shownAtMs = 0;
        impl_->clear = true;
    }
    impl_->cv.notify_one();
    std::lock_guard lock(impl_->audioMutex);
    if (impl_->audioDevice)
        SDL_ClearQueuedAudio(impl_->audioDevice);
    opus_decoder_ctl(impl_->opus, OPUS_RESET_STATE);
}
uint64_t Media::generation() const {
    return impl_->generation.load();
}
std::string Media::status() {
    std::lock_guard lock(impl_->mutex);
    auto status = impl_->error;
    if (impl_->decoderFailed.load())
        status = "视频解码已停止，请退出程序后重试";
    const auto audio = impl_->audioStatus.load();
    if (audio != Impl::AudioStatus::Ready) {
        if (!status.empty())
            status += " · ";
        status += audio == Impl::AudioStatus::Unavailable ? "音频设备不可用，请退出程序后重试"
                                                          : "音频播放失败，正在重试";
    }
    return status;
}
} // namespace uu
