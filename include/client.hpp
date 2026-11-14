#pragma once
#include "auth.hpp"
#include "media.hpp"
#include "transport.hpp"
#include <functional>
#include <rtc/rtc.hpp>
#include <set>

namespace uu {
struct Snapshot {
    std::string stage = "idle", detail = "按 A 扫码登录，或按 X 使用短信验证码", qrUrl, nickname, pairType;
    Json devices = Json::array();
    bool authenticated = false, streaming = false;
    int width = 1280, height = 720, fps = 30;
    std::string loginMode = "qr";
    int64_t smsRetryAt = 0;
};
class Client {
    struct Event {
        std::string type;
        Json data;
        uint64_t generation = 0;
    };
    struct Inbox {
        std::mutex mutex;
        std::condition_variable cv;
        std::deque<Event> events;
    };
    std::shared_ptr<Inbox> inbox_ = std::make_shared<Inbox>();
    std::shared_ptr<Media> media_;
    std::atomic<bool> stop_{false}, cancel_{false};
    std::thread worker_;
    std::mutex stateMutex_;
    Snapshot state_;
    std::unique_ptr<Api> api_;
    std::string sessionPath_;
    std::unique_ptr<SignalSocket> socket_;
    std::shared_ptr<rtc::PeerConnection> peer_;
    rtc::PeerConnection::IceState iceState_ = rtc::PeerConnection::IceState::New;
    std::map<std::string, std::shared_ptr<rtc::DataChannel>> channels_;
    std::shared_ptr<rtc::Track> video_, audio_;
    // 按协商结果重建视频解包链（H.264 / H.265），参数 true = HEVC。
    std::function<void(bool)> attachVideo_;
    bool videoHevc_ = false; // 协商结果是否使用 H.265
    std::atomic<int> videoPt_{-1}; // 视频 RTP 实际使用的 payload type
    Json remote_, ice_;
    std::string control_;
    int reqFps_ = 30, reqQuality_ = 1;
    uint64_t reqCustomBps_ = 4000000;
    uint64_t generation_ = 1;
    int nextAck_ = 1;
    std::map<int, std::string> acks_;
    std::set<int> heldKeys_, heldButtons_;
    std::deque<Json> earlySoac_;
    std::deque<rtc::Candidate> pendingCandidates_;
    SioPacket pending_;
    int remainingAttachments_ = 0;
    int64_t deadline_ = 0, lastPacket_ = 0, heartbeatTimeout_ = 45000, qrExpires_ = 0, qrNext_ = 0;
    int64_t lastFeedback_ = 0;
    int64_t lastEchoMs_ = 0;
    std::atomic<uint64_t> nextSeq_{1};
    std::atomic<uint64_t> pendingEchoSeq_{0};
    bool rpcSent_ = false, appliedLogged_ = false, qosLogged_ = false, echoReplied_ = false;
    Json qr_;
    int qrStatus_ = 1;
    SmsAuth sms_;
    void run();
    void drainEvents();
    void handle(const Event &event);
    void handleControlData(const uint8_t *data, size_t size);
    void sendRpcSettings();
    void packet(const WsMessage &message);
    void dispatch(const SioPacket &packet);
    void beginPeer(const Json &control);
    void handleSoac(const Json &data);
    void emit(const std::string &name, const Json &data, const std::string &ack = "",
              const Bytes &attachment = {});
    void setState(const std::string &stage, const std::string &detail);
    void refresh();
    void disconnect();
    void input(const Json &data);
    void release();
    void pollQr();
    void completeLogin(const Json &response);
    void tryRestore();
    void post(std::string type, Json data = nullptr, uint64_t generation = 0);

  public:
    Client(std::string key, std::string client, std::shared_ptr<Media> media,
           std::string sessionPath = {});
    ~Client();
    Snapshot snapshot();
    void command(const std::string &name, Json data = nullptr);
};
} // namespace uu
