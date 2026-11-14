#include "protocol.hpp"
#include <cctype>
#include <chrono>
#include <mbedtls/base64.h>
#include <mbedtls/md.h>
#include <random>
#include <stdexcept>
#ifdef __SWITCH__
#include <switch.h>
#endif

namespace uu {
int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}
int64_t monotonicMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
static std::string hex(const uint8_t *bytes, size_t size) {
    static constexpr char chars[] = "0123456789abcdef";
    std::string out;
    out.reserve(size * 2);
    for (size_t i = 0; i < size; ++i) {
        out += chars[bytes[i] >> 4];
        out += chars[bytes[i] & 15];
    }
    return out;
}
Bytes randomBytes(size_t size) {
    Bytes b(size);
#ifdef __SWITCH__
    randomGet(b.data(), b.size());
#else
    std::random_device random;
    for (auto &v : b)
        v = static_cast<uint8_t>(random());
#endif
    return b;
}
std::string uuid() {
    auto b = randomBytes(16);
    b[6] = (b[6] & 15) | 0x40;
    b[8] = (b[8] & 63) | 0x80;
    auto h = hex(b.data(), b.size());
    return h.substr(0, 8) + "-" + h.substr(8, 4) + "-" + h.substr(12, 4) + "-" + h.substr(16, 4) + "-" +
           h.substr(20);
}
std::string base64(const uint8_t *data, size_t size) {
    size_t len = 0;
    std::string out(4 * ((size + 2) / 3) + 1, '\0');
    if (mbedtls_base64_encode(reinterpret_cast<unsigned char *>(out.data()), out.size(), &len, data, size))
        throw std::runtime_error("Base64 encoding failed");
    out.resize(len);
    return out;
}
std::string percentEncode(const std::string &s) {
    std::string out;
    const char *h = "0123456789ABCDEF";
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
            out += c;
        else {
            out += '%';
            out += h[c >> 4];
            out += h[c & 15];
        }
    }
    return out;
}
std::string sign(const std::string &key, const std::string &method, const std::string &path,
                 const Headers &headers, const std::string &body) {
    Headers canonical;
    for (const auto &[name, value] : headers) {
        auto n = name;
        std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return std::tolower(c); });
        if (n.rfind("x-param-", 0) == 0 && n != "x-param-sign")
            canonical[n] = value;
    }
    std::string text = method + path;
    bool first = true;
    for (const auto &[name, value] : canonical) {
        if (!first)
            text += '&';
        first = false;
        text += name + '=' + value;
    }
    text += body;
    uint8_t digest[32];
    if (mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
                        reinterpret_cast<const uint8_t *>(key.data()), key.size(),
                        reinterpret_cast<const uint8_t *>(text.data()), text.size(), digest))
        throw std::runtime_error("Request signing failed");
    return hex(digest, 32);
}
Headers requestHeaders(const std::string &key, const std::string &method, const std::string &path,
                       const std::string &body, const std::string &client, const std::string &device,
                       const std::string &user, int64_t timestamp) {
    // PC 客户端身份（plat=1）：TV/安卓身份会被服务器按大屏弱网策略限制码率，
    // 且"控制端版本较低"比对的是这里的 VN 与被控端版本。
    Headers h = {{"X-Param-CHN", "gwqd"},
                 {"X-Param-OPR", ""},
                 {"X-Param-PKGN", "com.netease.uuremote"},
                 {"X-Param-PLAT", "1"},
                 {"X-Param-REL", "prod"},
                 {"X-Param-TS", std::to_string(timestamp)},
                 {"X-Param-VC", "44604716"},
                 {"X-Param-VN", "4.46.0.4716"},
                 {"X-Param-ABI", "x86_64"},
                 {"X-Param-client-id", client},
                 {"X-Param-device-id", device},
                 {"X-Param-user-id", user},
                 {"X-Param-OAID", ""},
                 {"X-Param-CNT", "CN"},
                 {"X-Param-LANG", "zh-CN"}};
    h["X-Param-SIGN"] = sign(key, method, path, h, body);
    h["Content-Type"] = "application/json; charset=utf-8";
    return h;
}
static void varint(Bytes &b, uint64_t n) {
    while (n > 127) {
        b.push_back((n & 127) | 128);
        n >>= 7;
    }
    b.push_back(n);
}
static void integer(Bytes &b, int field, uint64_t n) {
    varint(b, field * 8);
    varint(b, n);
}
static void message(Bytes &b, int field, const Bytes &s) {
    varint(b, field * 8 + 2);
    varint(b, s.size());
    b.insert(b.end(), s.begin(), s.end());
}
static void text(Bytes &b, int field, const std::string &s) {
    message(b, field, Bytes(s.begin(), s.end()));
}
// 字段号与取值对齐已知能在官方服务器跑通原画的客户端（linux-uuremote viewer，
// platform=4 Windows 身份）。platform=2 是安卓/TV 语义，会被套大屏弱网码率策略。
// 二分实验：此函数回滚到 0.1.26 已验证形状（该形状所有版本视频正常）。
// 0.1.27 的重写版（字段 6 空/删 3/加 9/platform=4 等）导致服务器不再推流。
Bytes connectOptions(int w, int h, int fps, int quality, uint64_t customBps,
                     const std::string &device) {
    (void)device;
    Bytes resolution, capture, decoder, out;
    integer(resolution, 1, w);
    integer(resolution, 2, h);
    integer(capture, 1, fps == 30 ? 1 : 2);
    integer(capture, 2, quality);
    integer(capture, 3, 1);
    integer(capture, 4, 2);
    message(capture, 5, resolution);
    message(capture, 6, resolution);
    integer(capture, 7, 1); // chroma 4:2:0
    if (quality == 6 && customBps)
        integer(capture, 8, customBps);
    integer(capture, 11, fps);
    integer(decoder, 1, 144); // 解码能力上限（服务器按此下发码率策略）
    integer(decoder, 2, 1);
    integer(decoder, 3, 1920);
    integer(decoder, 4, 1080);
    integer(decoder, 5, 1);
    integer(out, 1, 1);
    integer(out, 2, uint64_t(-1));
    message(out, 3, capture);
    message(out, 4, decoder);
    integer(out, 8, 4); // platform = Windows（单变量实验）
    Bytes feature;
    integer(feature, 1, 4);
    message(out, 11, feature);
    text(out, 12, "4.46.0.4716");
    return out;
}

std::vector<WireField> decodeWire(const Bytes &data) {
    std::vector<WireField> fields;
    size_t pos = 0;
    auto readVarint = [&](uint64_t &value) {
        value = 0;
        int shift = 0;
        while (pos < data.size() && shift < 64) {
            uint8_t b = data[pos++];
            value |= uint64_t(b & 0x7F) << shift;
            if (!(b & 0x80))
                return true;
            shift += 7;
        }
        return false;
    };
    while (pos < data.size()) {
        uint64_t key = 0;
        if (!readVarint(key))
            return fields;
        WireField f;
        f.number = int(key >> 3);
        int wiretype = int(key & 7);
        if (wiretype == 0) {
            if (!readVarint(f.varint))
                return fields;
        } else if (wiretype == 2) {
            uint64_t len = 0;
            if (!readVarint(len) || pos + len > data.size())
                return fields;
            f.bytes.assign(data.begin() + pos, data.begin() + pos + len);
            pos += len;
            f.isBytes = true;
        } else {
            return fields; // 未知 wiretype，放弃剩余
        }
        fields.push_back(std::move(f));
    }
    return fields;
}
Bytes sessionFeatures() {
    Bytes f;
    integer(f, 1, 6);
    integer(f, 2, 2);
    integer(f, 3, 1);
    integer(f, 4, 2);
    integer(f, 6, 2);
    integer(f, 7, 2);
    integer(f, 9, 1);
    return f;
}
Bytes envelope(uint64_t seq, int kind, const Bytes &payload) {
    Bytes out;
    integer(out, 1, seq);
    message(out, kind, payload);
    return out;
}
Bytes echoRequest(uint64_t seq, const Bytes &features) {
    Bytes action;
    integer(action, 1, 0); // action 0 = echo 请求
    message(action, 4, features);
    return envelope(seq, 3, action);
}
Bytes echoResponse(uint64_t seq, uint64_t requestSeq, const Bytes &features) {
    Bytes action;
    integer(action, 1, 1); // action 1 = echo 应答
    text(action, 2, "{\"seq\":" + std::to_string(requestSeq) + "}");
    message(action, 4, features);
    return envelope(seq, 3, action);
}
Bytes rpcQuality(uint64_t seq, uint64_t requestId, int quality) {
    Bytes body;
    integer(body, 1, uint64_t(quality));
    Bytes header;
    integer(header, 1, requestId);
    Bytes rpc;
    message(rpc, 1, header);
    message(rpc, 4, body);
    return envelope(seq, 21, rpc);
}
Bytes rpcFps(uint64_t seq, uint64_t requestId, int fps) {
    Bytes body;
    integer(body, 1, uint64_t(fps <= 30 ? 1 : 2)); // 菜单序号：1=30 2=60
    Bytes header;
    integer(header, 1, requestId);
    Bytes rpc;
    message(rpc, 1, header);
    message(rpc, 3, body);
    return envelope(seq, 21, rpc);
}
// case 2 采集设置：被控端收到它才开始抓屏编码（官方客户端连接后必发）。
Bytes rpcCaptureSettings(uint64_t seq, uint64_t requestId, int quality, int fps,
                         uint64_t customBps) {
    Bytes body;
    integer(body, 1, uint64_t(fps <= 30 ? 1 : 2)); // 帧率菜单序号
    integer(body, 2, uint64_t(quality));
    integer(body, 5, 1280); // local_resolution
    integer(body, 6, 720);
    integer(body, 7, 1); // chroma 4:2:0
    if (quality == 6 && customBps)
        integer(body, 8, customBps);
    integer(body, 10, 2); // choose_resolution_type = 本地自适应
    integer(body, 13, 1); // codec = H264
    integer(body, 18, uint64_t(fps)); // fpsCount 真实帧率
    Bytes header;
    integer(header, 1, requestId);
    Bytes rpc;
    message(rpc, 1, header);
    message(rpc, 2, body);
    return envelope(seq, 21, rpc);
}
Bytes launchApp(uint64_t seq, const Json &app) {
    Bytes p, out;
    text(p, 1, app.value("dapp_id", std::string{}));
    text(p, 2, app.value("name", std::string{}));
    text(p, 3, app.value("target_path", std::string{}));
    integer(out, 1, seq);
    integer(out, 2, nowMs());
    message(out, 4, p);
    return out;
}
Json ackValue(const Json &args, const std::string &event) {
    if (!args.is_array() || args.empty())
        throw std::runtime_error(event + ": empty acknowledgment");
    Json value;
    if (args.size() == 1 && args[0].is_object())
        value = args[0];
    else {
        auto e = args[0];
        if (!e.is_null() && e != false && e != 0 && e != "" && e != "succ" && e != "success" &&
            !(e.is_object() && e.value("code", -1) == 0))
            throw std::runtime_error(event + ": request rejected");
        if (args.size() < 2)
            throw std::runtime_error(event + ": missing response");
        value = args[1];
    }
    if (value.is_object() && value.contains("code") && value["code"] != 0)
        throw std::runtime_error(event + ": " + value.value("msg", std::string("request rejected")));
    return value;
}
Json soac(const Json &client, const std::string &type, const Json &payload, const Json &ice,
          const std::string &control) {
    Json data = type == "candidate" ? Json{{"candidate", payload}} : payload;
    data["type"] = type;
    data["ice_id"] = ice;
    data["app_control_id"] = control;
    return {{"client_id", client}, {"data", data}};
}
std::string signalingUrl(const std::string &source) {
    std::string s = source.find("://") == std::string::npos ? "wss://" + source : source;
    if (s.rfind("wss://", 0) != 0)
        throw std::runtime_error("Signaling requires WSS");
    auto end = s.find_first_of("/?#", 6);
    auto authority = s.substr(6, end == std::string::npos ? end : end - 6);
    auto colon = authority.find(':');
    auto host = authority.substr(0, colon);
    std::transform(host.begin(), host.end(), host.begin(), [](unsigned char c) { return std::tolower(c); });
    if (host != "163.com" && (host.size() < 8 || host.substr(host.size() - 8) != ".163.com"))
        throw std::runtime_error("Untrusted signaling host");
    if (authority.find('@') != std::string::npos)
        throw std::runtime_error("Invalid signaling address");
    return "wss://" + authority + "/socket.io/?EIO=4&transport=websocket";
}
Bytes wsFrame(uint8_t opcode, const Bytes &payload, std::array<uint8_t, 4> mask) {
    Bytes b{uint8_t(0x80 | opcode)};
    size_t n = payload.size();
    if (n < 126)
        b.push_back(0x80 | n);
    else if (n <= 65535) {
        b.push_back(0xfe);
        b.push_back(n >> 8);
        b.push_back(n);
    } else {
        b.push_back(0xff);
        for (int i = 7; i >= 0; --i)
            b.push_back(uint64_t(n) >> (i * 8));
    }
    b.insert(b.end(), mask.begin(), mask.end());
    for (size_t i = 0; i < n; ++i)
        b.push_back(payload[i] ^ mask[i % 4]);
    return b;
}
std::vector<WsMessage> WsParser::feed(const uint8_t *data, size_t size) {
    constexpr size_t max = 2 * 1024 * 1024;
    if (pending_.size() + size > max + 14)
        throw std::runtime_error("WebSocket buffer limit");
    pending_.insert(pending_.end(), data, data + size);
    std::vector<WsMessage> out;
    size_t p = 0;
    while (pending_.size() - p >= 2) {
        auto *b = pending_.data() + p;
        bool fin = b[0] & 128;
        uint8_t op = b[0] & 15;
        size_t head = 2;
        uint64_t n = b[1] & 127;
        if ((b[0] & 0x70) || (b[1] & 128))
            throw std::runtime_error("Invalid server WebSocket frame");
        if (n == 126) {
            if (pending_.size() - p < 4)
                break;
            n = (b[2] << 8) | b[3];
            head = 4;
        } else if (n == 127) {
            if (pending_.size() - p < 10)
                break;
            n = 0;
            for (int i = 2; i < 10; ++i)
                n = (n << 8) | b[i];
            head = 10;
        }
        if (n > max || (op >= 8 && (!fin || n > 125)))
            throw std::runtime_error("Invalid WebSocket length");
        if (n > pending_.size() - p - head)
            break;
        Bytes payload(b + head, b + head + n);
        p += head + n;
        if (op >= 8) {
            if (op != 8 && op != 9 && op != 10)
                throw std::runtime_error("Invalid control opcode");
            out.push_back({op, std::move(payload)});
        } else if (op == 0) {
            if (!fragmentOpcode_)
                throw std::runtime_error("Unexpected continuation");
            if (fragment_.size() + n > max)
                throw std::runtime_error("Fragment limit");
            fragment_.insert(fragment_.end(), payload.begin(), payload.end());
            if (fin) {
                out.push_back({fragmentOpcode_, std::move(fragment_)});
                fragment_.clear();
                fragmentOpcode_ = 0;
            }
        } else if (op == 1 || op == 2) {
            if (fragmentOpcode_)
                throw std::runtime_error("Interleaved WebSocket message");
            if (fin)
                out.push_back({op, std::move(payload)});
            else {
                fragmentOpcode_ = op;
                fragment_ = std::move(payload);
            }
        } else
            throw std::runtime_error("Invalid WebSocket opcode");
    }
    pending_.erase(pending_.begin(), pending_.begin() + p);
    return out;
}
SioPacket parseSocketIo(const std::string &s) {
    SioPacket p;
    if (s.size() < 2 || s[0] != '4' || s[1] < '0' || s[1] > '6')
        throw std::runtime_error("Invalid Socket.IO packet");
    p.type = s[1] - '0';
    size_t i = 2;
    auto number = [&]() {
        int n = 0;
        size_t start = i;
        while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) {
            if (n > 1000000)
                throw std::runtime_error("Socket.IO number limit");
            n = n * 10 + s[i++] - '0';
        }
        if (i == start)
            throw std::runtime_error("Missing Socket.IO number");
        return n;
    };
    if (p.type == 5 || p.type == 6) {
        p.attachments = number();
        if (p.attachments > 16 || i >= s.size() || s[i++] != '-')
            throw std::runtime_error("Invalid attachments");
    }
    if (i < s.size() && s[i] == '/')
        throw std::runtime_error("Unsupported Socket.IO namespace");
    if (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i])))
        p.id = number();
    if (i < s.size())
        p.data = Json::parse(s.substr(i));
    return p;
}
} // namespace uu
