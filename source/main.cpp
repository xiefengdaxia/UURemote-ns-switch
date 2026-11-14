#include "background.hpp"
#include "client.hpp"
#include "devsupport.hpp"
#include "diagnostics.hpp"
#include "qrcodegen.hpp"
#include "rtc_diagnostics.hpp"
#include <SDL_ttf.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <exception>
#include <mutex>
#include <thread>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <sys/stat.h>
#ifdef __SWITCH__
#include <switch.h>
#endif

// Visual thesis: quiet dark workspace, large readable type, a single mint focus.
// Content: account and connection status, computer list, QR/desktop workspace, controls.
// Interaction: eased focus movement, a restrained busy pulse, toggleable stream overlay.
namespace {
constexpr SDL_Color white{238, 243, 246, 255}, muted{142, 158, 169, 255}, accent{102, 231, 197, 255};
void color(SDL_Renderer *r, SDL_Color c) {
    SDL_SetRenderDrawColor(r, c.r, c.g, c.b, c.a);
}
void rect(SDL_Renderer *r, int x, int y, int w, int h, SDL_Color c) {
    color(r, c);
    SDL_Rect a{x, y, w, h};
    SDL_RenderFillRect(r, &a);
}
// 电脑端画面里捕获不到系统光标时，本地画一个标准箭头（白填充+黑描边，
// 亮暗背景下都可见），比十字标记更接近桌面原生的观感。
void drawCursor(SDL_Renderer *r, int x, int y) {
    static const char *const kMask[] = {
        "X...........", "XX..........", "X#X.........", "X##X........",
        "X###X.......", "X####X......", "X#####X.....", "X######X....",
        "X#######X...", "X########X..", "X#####XXXXX.", "X##X##X.....",
        "X#X.X##X....", "XX..X##X....", "X....X##X...", ".....X##X...",
        "......X##X..", "......X##X..", ".......XX...",
    };
    constexpr int cell = 2;
    constexpr SDL_Color outline{0, 0, 0, 255}, fill{255, 255, 255, 255};
    const int rows = int(sizeof kMask / sizeof kMask[0]);
    for (int pass = 0; pass < 2; ++pass) {
        color(r, pass == 0 ? outline : fill);
        const char mark = pass == 0 ? 'X' : '#';
        for (int row = 0; row < rows; ++row) {
            for (int col = 0; kMask[row][col]; ++col) {
                if (kMask[row][col] != mark)
                    continue;
                SDL_Rect a{x + col * cell, y + row * cell, cell, cell};
                SDL_RenderFillRect(r, &a);
            }
        }
    }
}
void text(SDL_Renderer *r, TTF_Font *f, const std::string &s, int x, int y, SDL_Color c = white,
          int maxWidth = 1200) {
    if (s.empty())
        return;
    auto *surface = TTF_RenderUTF8_Blended(f, s.c_str(), c);
    if (!surface)
        return;
    auto *t = SDL_CreateTextureFromSurface(r, surface);
    SDL_Rect dst{x, y, surface->w, surface->h};
    if (dst.w > maxWidth) {
        SDL_Rect src{0, 0, maxWidth, surface->h};
        dst.w = maxWidth;
        SDL_RenderCopy(r, t, &src, &dst);
    } else
        SDL_RenderCopy(r, t, nullptr, &dst);
    SDL_DestroyTexture(t);
    SDL_FreeSurface(surface);
}
void statusText(SDL_Renderer *renderer, TTF_Font *font, const std::string &message, SDL_Color tint) {
    auto *surface = TTF_RenderUTF8_Blended_Wrapped(font, message.substr(0, 512).c_str(), tint, 1110);
    if (!surface)
        return;
    auto *texture = SDL_CreateTextureFromSurface(renderer, surface);
    SDL_Rect source{0, 0, surface->w, std::min(surface->h, 64)}, destination{82, 580, source.w, source.h};
    if (texture)
        SDL_RenderCopy(renderer, texture, &source, &destination);
    SDL_DestroyTexture(texture);
    SDL_FreeSurface(surface);
}
std::optional<std::string> numericInput(const char *title, const std::string &value, int minimum,
                                        int maximum) {
#ifdef __SWITCH__
    SwkbdConfig keyboard{};
    if (R_FAILED(swkbdCreate(&keyboard, 0)))
        return std::nullopt;
    swkbdConfigMakePresetDefault(&keyboard);
    swkbdConfigSetType(&keyboard, SwkbdType_NumPad);
    swkbdConfigSetHeaderText(&keyboard, title);
    swkbdConfigSetInitialText(&keyboard, value.c_str());
    swkbdConfigSetStringLenMin(&keyboard, minimum);
    swkbdConfigSetStringLenMax(&keyboard, maximum);
    swkbdConfigSetOkButtonText(&keyboard, "确定");
    std::array<char, 64> buffer{};
    auto result = swkbdShow(&keyboard, buffer.data(), buffer.size());
    swkbdClose(&keyboard);
    if (R_FAILED(result))
        return std::nullopt;
    return std::string(buffer.data());
#else
    (void)title;
    (void)value;
    (void)minimum;
    (void)maximum;
    return std::nullopt;
#endif
}
struct SmsForm {
    std::string country = "86", mobile, code;
    int selected = 1;
    uu::Json credentials() const {
        return {{"countryCode", country}, {"mobile", mobile}, {"code", code}};
    }
};
struct Fonts {
    TTF_Font *normal = nullptr, *small = nullptr, *large = nullptr;
    Fonts() {
#ifdef __SWITCH__
        PlFontData data{};
        if (R_FAILED(plGetSharedFontByType(&data, PlSharedFontType_ChineseSimplified)))
            throw std::runtime_error("Cannot load system font");
        auto load = [&](int size) {
            return TTF_OpenFontRW(SDL_RWFromConstMem(data.address, int(data.size)), 1, size);
        };
#else
        const char *env = std::getenv("UU_FONT");
        std::string path = env ? env : "C:/Windows/Fonts/msyh.ttc";
        auto load = [&](int size) { return TTF_OpenFont(path.c_str(), size); };
#endif
        normal = load(26);
        small = load(21);
        large = load(48);
        if (!normal || !small || !large)
            throw std::runtime_error(TTF_GetError());
    }
    ~Fonts() {
        TTF_CloseFont(normal);
        TTF_CloseFont(small);
        TTF_CloseFont(large);
    }
};
std::string read(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), {});
}
struct Controls {
    bool confirm = false, back = false, refresh = false, logout = false, up = false, down = false,
         help = false, disconnect = false, exit = false, qualitySwitch = false, fpsSwitch = false,
         bitrateUp = false, bitrateDown = false, hud = false, touchToggle = false;
    float mx = 0, my = 0, scroll = 0;
    uint64_t held = 0;
};
#ifdef __SWITCH__
struct FocusHook {
    AppletHookCookie cookie{};
    explicit FocusHook(uu::Client &client) {
        appletHook(
            &cookie,
            [](AppletHookType type, void *data) {
                if ((type == AppletHookType_OnFocusState &&
                     appletGetFocusState() != AppletFocusState_InFocus) ||
                    type == AppletHookType_OnExitRequest) {
                    static_cast<uu::Client *>(data)->command("release");
                }
            },
            &client);
    }
    ~FocusHook() {
        appletUnhook(&cookie);
    }
};
#endif
} // namespace

int main(int, char **) {
#ifdef __SWITCH__
    if (appletGetAppletType() != AppletType_Application &&
        appletGetAppletType() != AppletType_SystemApplication) {
        consoleInit(nullptr);
        printf("UU Remote NS requires full application mode.\nHold R while launching a game, then start this "
               "NRO.\nPress + to exit.\n");
        PadState p;
        padConfigureInput(1, HidNpadStyleSet_NpadStandard);
        padInitializeDefault(&p);
        while (appletMainLoop()) {
            padUpdate(&p);
            if (padGetButtonsDown(&p) & HidNpadButton_Plus)
                break;
            consoleUpdate(nullptr);
        }
        consoleExit(nullptr);
        return 0;
    }
    // 默认 UDP 接收缓冲只有 0xA500（42KB）——视频按帧突发到达，网络线程稍有
    // 调度延迟就会溢出丢包（实测 4~8%），进而触发 IDR/降码率。这里提高接收
    // 缓冲池；申请失败则回退默认配置，保证网络可用。
    {
        SocketInitConfig cfg = *socketGetDefaultInitConfig();
        cfg.udp_rx_buf_size = 0x40000; // 256KB
        if (R_FAILED(socketInitialize(&cfg)))
            socketInitializeDefault();
    }
    nifmInitialize(NifmServiceType_User);
    sslInitialize(0);
    romfsInit();
    plInitialize(PlServiceType_User);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);
    hidInitializeTouchScreen();
    appletSetMediaPlaybackState(true);
#endif
    int result = 0;
    try {
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS) != 0)
            throw std::runtime_error(SDL_GetError());
        if (TTF_Init() != 0)
            throw std::runtime_error(TTF_GetError());
        curl_global_init(CURL_GLOBAL_DEFAULT);
        {
            auto *window = SDL_CreateWindow("UU Remote NS", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                            1280, 720, SDL_WINDOW_SHOWN);
            if (!window)
                throw std::runtime_error(SDL_GetError());
            auto *renderer =
                SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
            if (!renderer)
                renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
            if (!renderer)
                throw std::runtime_error(SDL_GetError());
            SDL_RenderSetLogicalSize(renderer, 1280, 720);
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
            Fonts fonts;
            // 签名 key 不随源码/发行包分发：优先读 SD 卡（用户自行从自己的 APK 提取），
            // 其次读构建时打入 RomFS 的副本；都没有则拒绝启动。
#ifdef __SWITCH__
            std::string key = read("sdmc:/switch/UURemote-ns/signing.key");
            if (key.size() != 24)
                key = read("romfs:/signing.key");
            const std::string profile = "sdmc:/switch/UURemote-ns/client-id";
            mkdir("sdmc:/switch/UURemote-ns", 0777);
#else
            std::string key = read("signing.key");
            if (key.size() != 24)
                key = read("romfs/signing.key");
            const std::string profile = "client-id";
#endif
            if (key.size() != 24)
                throw std::runtime_error(
                    "缺少签名 key：请用 scripts/extract_key.py 从自己的 UU TV APK 提取到 "
                    "sdmc:/switch/UURemote-ns/signing.key");
            uu::diagnostic("app-start-" UU_VERSION);
            std::set_terminate(uu::terminateWithDiagnostics);
            rtc::InitLogger(rtc::LogLevel::Info, [](rtc::LogLevel, const std::string &message) {
                const auto event = uu::rtcLogEvent(message);
                if (!event.empty())
                    uu::diagnostic(event.c_str());
            });
            std::string id = read(profile);
            if (id.size() != 36) {
                id = uu::uuid();
                std::ofstream(profile) << id;
            }
            const std::string settingsPath =
#ifdef __SWITCH__
                "sdmc:/switch/UURemote-ns/settings";
#else
                "settings";
#endif
            const std::string sessionPath =
#ifdef __SWITCH__
                "sdmc:/switch/UURemote-ns/session";
#else
                "session";
#endif
            int fps = 30, quality = 1, bitrateIdx = 3;
            static const int bitrateSteps[] = {1, 2, 3, 4, 6, 8, 10, 15, 20}; // Mbps，画质=自定义时生效
            {
                std::istringstream saved(read(settingsPath));
                int f = 0, q = 0, b = 3;
                if (saved >> f >> q && (f == 30 || f == 60) && q >= 1 && q <= 6) {
                    if (!(saved >> b) || b < 0 || b > 8)
                        b = 3;
                    fps = f;
                    quality = q;
                    bitrateIdx = b;
                }
            }
            auto saveSettings = [&] {
                std::ofstream(settingsPath) << fps << ' ' << quality << ' ' << bitrateIdx << '\n';
            };
            auto media = std::make_shared<uu::Media>();
            {
                uu::Client client(key, id, media, sessionPath);
#ifdef __SWITCH__
                FocusHook focusHook(client);
#endif
                bool running = true, overlay = true, wasStreaming = false, touchHeld = false,
                     hudVisible = true;
                int selected = 0, texW = 0, texH = 0;
                uint64_t serial = 0, lastHeld = 0;
                int diagFrames = 0, diagFps = 0;
                long long diagLastMs = 0;
                float cursorX = .5f, cursorY = .5f, focusY = 230;
                // 触屏模式：单指 = 绝对定位 + 左键拖动，双指 = 滚动，长按 = 右键
                bool touchMode = false, fingerDown = false, twoFinger = false, buttonHeld = false;
                bool fingerMoved = false, longPressFired = false;
                float lastAbsX = .5f, lastAbsY = .5f, lastTouchX = 0, lastTouchY = 0;
                float fingerStartX = 0, fingerStartY = 0;
                Uint32 lastFingerMove = 0, lastTwoFinger = 0, fingerStartMs = 0, lastScrollDiag = 0;
                int touchDotX = -1, touchDotY = -1;
                Uint32 previous = SDL_GetTicks(), lastScroll = 0, lastPointer = 0;
                SDL_Texture *video = nullptr;
                std::string qrUrl;
                std::unique_ptr<qrcodegen::QrCode> qr;
                SmsForm sms;
                SDL_Rect viewport{0, 0, 1280, 720};
                auto send = [&](const uu::Json &data) { client.command("input", data); };
                std::atomic<bool> devStop{false};
                std::mutex devNoticeMutex;
                std::string devNotice;
                std::thread devThread([&] {
                    auto bases = uu::loadDevServers();
                    int waited = 0;
                    while (!devStop.load()) {
                        std::this_thread::sleep_for(std::chrono::seconds(1));
                        if (++waited < 15 || bases.empty())
                            continue;
                        waited = 0;
                        auto ver = uu::devTick(bases,
#ifdef __SWITCH__
                                               "sdmc:/switch/UURemote-ns/connection.log",
                                               "sdmc:/switch/UURemote-ns/UURemote-ns.nro");
#else
                                               "connection.log", "UURemote-ns.nro");
#endif
                        if (!ver.empty()) {
                            std::lock_guard lk(devNoticeMutex);
                            devNotice = "已自动更新到 v" + ver + "，按 + 退出再重新打开生效";
                        }
                    }
                });
                while (running) {
#ifdef __SWITCH__
                    if (!appletMainLoop())
                        break;
#endif
                    Uint32 now = SDL_GetTicks();
                    float dt = std::min((now - previous) / 1000.f, .05f);
                    previous = now;
                    auto state = client.snapshot();
                    selected = std::clamp(selected, 0, std::max(0, int(state.devices.size()) - 1));
                    Controls c;
                    SDL_Event event;
                    while (SDL_PollEvent(&event)) {
                        if (event.type == SDL_QUIT)
                            running = false;
                        if (event.type == SDL_WINDOWEVENT &&
                            event.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
                            client.command("release");
                            touchHeld = false;
                        }
                        if (event.type == SDL_KEYDOWN && !event.key.repeat) {
                            auto k = event.key.keysym.sym;
                            c.confirm |= k == SDLK_RETURN;
                            c.back |= k == SDLK_ESCAPE;
                            c.refresh |= k == SDLK_y;
                            c.logout |= k == SDLK_x;
                            c.up |= k == SDLK_UP;
                            c.down |= k == SDLK_DOWN;
                            c.help |= k == SDLK_F1;
                            c.disconnect |= k == SDLK_F2;
                            c.exit |= k == SDLK_F10;
                        }
#ifndef __SWITCH__
                        if (state.streaming &&
                            (event.type == SDL_MOUSEMOTION || event.type == SDL_MOUSEBUTTONDOWN ||
                             event.type == SDL_MOUSEBUTTONUP)) {
                            if (event.type == SDL_MOUSEMOTION) {
                                cursorX =
                                    std::clamp((event.motion.x - viewport.x) / float(viewport.w), 0.f, 1.f);
                                cursorY =
                                    std::clamp((event.motion.y - viewport.y) / float(viewport.h), 0.f, 1.f);
                                send({{"action", "mouse_move_absolute"},
                                      {"abs_x", cursorX},
                                      {"abs_y", cursorY}});
                            } else
                                send({{"action",
                                       event.type == SDL_MOUSEBUTTONDOWN ? "mouse_press" : "mouse_release"},
                                      {"button", event.button.button == SDL_BUTTON_LEFT ? 1 : 2}});
                        }
#endif
                    }
#ifdef __SWITCH__
                    padUpdate(&pad);
                    uint64_t pressed = padGetButtonsDown(&pad);
                    c.held = padGetButtons(&pad);
                    c.confirm |= pressed & HidNpadButton_A;
                    c.back |= pressed & HidNpadButton_B;
                    c.refresh |= pressed & HidNpadButton_Y;
                    c.logout |= pressed & HidNpadButton_X;
                    c.up |= pressed & HidNpadButton_Up;
                    c.down |= pressed & HidNpadButton_Down;
                    c.help |= pressed & HidNpadButton_Plus;
                    c.disconnect |= pressed & HidNpadButton_Minus;
                    c.qualitySwitch |= pressed & HidNpadButton_L;
                    c.fpsSwitch |= pressed & HidNpadButton_R;
                    c.bitrateUp |= pressed & HidNpadButton_ZR;
                    c.bitrateDown |= pressed & HidNpadButton_ZL;
                    c.hud |= pressed & HidNpadButton_StickL;
                    c.touchToggle |= pressed & HidNpadButton_StickR;
                    c.exit |= !state.streaming && (pressed & HidNpadButton_Plus);
                    auto stick = padGetStickPos(&pad, 0), right = padGetStickPos(&pad, 1);
                    auto axis = [](int v) {
                        float n = v / 32767.f;
                        return std::abs(n) > .15f ? std::copysign((std::abs(n) - .15f) / .85f, n) : 0.f;
                    };
                    c.mx = axis(stick.x);
                    c.my = -axis(stick.y);
                    c.scroll = axis(right.y);
#endif
                    bool busy = state.stage != "idle" && state.stage != "error" && state.stage != "login" &&
                                state.stage != "sms";
                    if (c.exit)
                        running = false;
                    if (state.streaming) {
                        if (c.help)
                            overlay = !overlay;
                        if (c.hud)
                            hudVisible = !hudVisible;
                        if (c.touchToggle) {
                            touchMode = !touchMode;
                            touchDotX = touchDotY = -1;
                            fingerDown = false;
                            twoFinger = false;
                            if (buttonHeld) {
                                send({{"action", "mouse_release"}, {"button", 1}});
                                buttonHeld = false;
                            }
                        }
                        if (c.disconnect) {
                            client.command("disconnect");
                            continue;
                        }
                        cursorX = std::clamp(cursorX + c.mx * dt * .7f, 0.f, 1.f);
                        cursorY = std::clamp(cursorY + c.my * dt * .7f, 0.f, 1.f);
                        if ((c.mx || c.my) && now - lastPointer >= 16) {
                            send({{"action", "mouse_move_absolute"}, {"abs_x", cursorX}, {"abs_y", cursorY}});
                            lastPointer = now;
                        }
                        if (c.scroll && now - lastScroll > 80) {
                            send({{"action", "mouse_scroll"},
                                  {"delta_x", 0},
                                  {"delta_y", int(c.scroll * 120)}});
                            lastScroll = now;
                        }
#ifdef __SWITCH__
                        for (auto [button, keycode] :
                             std::initializer_list<std::pair<uint64_t, int>>{{HidNpadButton_A, 13},
                                                                             {HidNpadButton_B, 27},
                                                                             {HidNpadButton_X, 32},
                                                                             {HidNpadButton_Y, 9},
                                                                             {HidNpadButton_L, 17},
                                                                             {HidNpadButton_R, 16},
                                                                             {HidNpadButton_Up, 87},
                                                                             {HidNpadButton_Down, 83},
                                                                             {HidNpadButton_Left, 65},
                                                                             {HidNpadButton_Right, 68}}) {
                            if ((c.held & button) != (lastHeld & button))
                                send({{"action", (c.held & button) ? "kbd_press" : "kbd_release"},
                                      {"key", keycode}});
                        }
                        for (auto [button, mouse] : std::initializer_list<std::pair<uint64_t, int>>{
                                 {HidNpadButton_ZR, 1}, {HidNpadButton_ZL, 2}})
                            if ((c.held & button) != (lastHeld & button))
                                send({{"action", (c.held & button) ? "mouse_press" : "mouse_release"},
                                      {"button", mouse}});
                        HidTouchScreenState touch{};
                        hidGetTouchScreenStates(&touch, 1);
                        const int touchCount = int(touch.count);
                        touchDotX = touchDotY = -1;
                        if (touchMode) {
                            // 触屏模式：单指 = 远端触控事件；双指 = 滚动；离开画面区域按抬起处理
                            auto normIn = [&](const HidTouchState &tp, float &ax, float &ay) {
                                const bool in = int(tp.x) >= viewport.x &&
                                                int(tp.x) < viewport.x + viewport.w &&
                                                int(tp.y) >= viewport.y &&
                                                int(tp.y) < viewport.y + viewport.h;
                                if (!in)
                                    return false;
                                ax = (tp.x - viewport.x) / float(viewport.w);
                                ay = (tp.y - viewport.y) / float(viewport.h);
                                return true;
                            };
                            if (touchCount >= 2) {
                                if (buttonHeld) {
                                    send({{"action", "mouse_release"}, {"button", 1}});
                                    buttonHeld = false;
                                }
                                fingerDown = false;
                                const float mx = (touch.touches[0].x + touch.touches[1].x) * .5f;
                                const float my = (touch.touches[0].y + touch.touches[1].y) * .5f;
                                touchDotX = int(mx);
                                touchDotY = int(my);
                                if (!twoFinger) {
                                    twoFinger = true;
                                    lastTouchX = mx;
                                    lastTouchY = my;
                                } else if (now - lastTwoFinger >= 16) {
                                    const int dx = int(mx - lastTouchX), dy = int(my - lastTouchY);
                                    lastTouchX = mx;
                                    lastTouchY = my;
                                    if (dx || dy) {
                                        lastTwoFinger = now;
                                        if (now - lastScrollDiag > 1000) {
                                            lastScrollDiag = now;
                                            char line[32];
                                            std::snprintf(line, sizeof line, "scroll-%d-%d", dx, dy);
                                            uu::diagnostic(line);
                                        }
                                        send({{"action", "two_fingers_scroll"},
                                              {"delta_x", dx},
                                              {"delta_y", dy}});
                                        send({{"action", "two_fingers_scroll"},
                                              {"delta_x", 0},
                                              {"delta_y", 0}});
                                    }
                                }
                            } else if (touchCount == 1) {
                                twoFinger = false;
                                float ax = 0, ay = 0;
                                if (normIn(touch.touches[0], ax, ay)) {
                                    touchDotX = int(touch.touches[0].x);
                                    touchDotY = int(touch.touches[0].y);
                                    lastAbsX = ax;
                                    lastAbsY = ay;
                                    cursorX = ax;
                                    cursorY = ay;
                                    // 触屏语义 = 绝对定位 + 左键按下（mouse_* 已验证可用）
                                    if (!fingerDown) {
                                        fingerDown = true;
                                        buttonHeld = true;
                                        lastFingerMove = now;
                                        fingerStartMs = now;
                                        fingerStartX = touch.touches[0].x;
                                        fingerStartY = touch.touches[0].y;
                                        fingerMoved = false;
                                        longPressFired = false;
                                        send({{"action", "mouse_move_absolute"},
                                              {"abs_x", ax},
                                              {"abs_y", ay}});
                                        send({{"action", "mouse_press"}, {"button", 1}});
                                    } else {
                                        if (std::abs(int(touch.touches[0].x) - int(fingerStartX)) > 10 ||
                                            std::abs(int(touch.touches[0].y) - int(fingerStartY)) > 10)
                                            fingerMoved = true;
                                        // 长按（650ms 内几乎没移动）= 右键
                                        if (!fingerMoved && !longPressFired &&
                                            now - fingerStartMs >= 650) {
                                            longPressFired = true;
                                            if (buttonHeld) {
                                                send({{"action", "mouse_release"}, {"button", 1}});
                                                buttonHeld = false;
                                            }
                                            send({{"action", "mouse_move_absolute"},
                                                  {"abs_x", ax},
                                                  {"abs_y", ay}});
                                            send({{"action", "mouse_press"}, {"button", 2}});
                                            send({{"action", "mouse_release"}, {"button", 2}});
                                        } else if (now - lastFingerMove >= 16) {
                                            lastFingerMove = now;
                                            send({{"action", "mouse_move_absolute"},
                                                  {"abs_x", ax},
                                                  {"abs_y", ay}});
                                        }
                                    }
                                } else if (fingerDown) {
                                    fingerDown = false;
                                    if (buttonHeld) {
                                        send({{"action", "mouse_release"}, {"button", 1}});
                                        buttonHeld = false;
                                    }
                                }
                            } else {
                                twoFinger = false;
                                fingerDown = false;
                                if (buttonHeld) {
                                    send({{"action", "mouse_release"}, {"button", 1}});
                                    buttonHeld = false;
                                }
                            }
                            touchHeld = false;
                        } else {
                            bool inside = false;
                            if (touchCount) {
                                auto p = touch.touches[0];
                                inside = int(p.x) >= viewport.x && int(p.x) < viewport.x + viewport.w &&
                                         int(p.y) >= viewport.y && int(p.y) < viewport.y + viewport.h;
                                if (inside) {
                                    cursorX = (p.x - viewport.x) / float(viewport.w);
                                    cursorY = (p.y - viewport.y) / float(viewport.h);
                                    send({{"action", "mouse_move_absolute"},
                                          {"abs_x", cursorX},
                                          {"abs_y", cursorY}});
                                }
                            }
                            if (inside != touchHeld) {
                                send({{"action", inside ? "mouse_press" : "mouse_release"},
                                      {"button", 1}});
                                touchHeld = inside;
                            }
                        }
#else
                        if (c.confirm) {
                            send({{"action", "kbd_press"}, {"key", 13}});
                            send({{"action", "kbd_release"}, {"key", 13}});
                        }
                        if (c.back) {
                            send({{"action", "kbd_press"}, {"key", 27}});
                            send({{"action", "kbd_release"}, {"key", 27}});
                        }
#endif
                    } else {
                        if (!busy && state.authenticated) {
                            if (c.qualitySwitch) {
                                quality = quality % 6 + 1;
                                saveSettings();
                            }
                            if (c.fpsSwitch) {
                                fps = fps == 30 ? 60 : 30;
                                saveSettings();
                            }
                            if (quality == 6) {
                                if (c.bitrateUp && bitrateIdx < 8) {
                                    bitrateIdx++;
                                    saveSettings();
                                }
                                if (c.bitrateDown && bitrateIdx > 0) {
                                    bitrateIdx--;
                                    saveSettings();
                                }
                            }
                        }
                        if (wasStreaming) {
                            if (video) {
                                SDL_DestroyTexture(video);
                                video = nullptr;
                            }
                            serial = 0;
                            texW = texH = 0;
                            diagFrames = diagFps = 0;
                            diagLastMs = 0;
                            overlay = true;
                            touchHeld = false;
                        }
                        if (!state.authenticated && c.logout && (!busy || state.stage == "login")) {
                            sms = SmsForm{};
                            client.command(state.loginMode == "sms" ? "login" : "sms-open");
                        } else if (c.back || c.disconnect) {
                            sms = SmsForm{};
                            client.command("cancel");
                        } else if (c.logout && state.authenticated)
                            client.command("logout");
                        else if (c.refresh && state.authenticated && !busy)
                            client.command("refresh");
                        else if (!state.authenticated && state.loginMode == "sms") {
                            if (!busy) {
                                if (c.up)
                                    sms.selected = std::max(0, sms.selected - 1);
                                if (c.down)
                                    sms.selected = std::min(4, sms.selected + 1);
                                if (c.confirm) {
                                    if (sms.selected < 3) {
                                        auto *value = sms.selected == 0   ? &sms.country
                                                      : sms.selected == 1 ? &sms.mobile
                                                                          : &sms.code;
                                        const char *title = sms.selected == 0   ? "国家或地区区号"
                                                            : sms.selected == 1 ? "手机号码"
                                                                                : "6 位短信验证码";
                                        auto input = numericInput(title, *value,
                                                                  sms.selected == 0   ? 1
                                                                  : sms.selected == 1 ? 4
                                                                                      : 6,
                                                                  sms.selected == 0   ? 4
                                                                  : sms.selected == 1 ? 15
                                                                                      : 6);
                                        if (input) {
                                            *value = *input;
                                            sms.selected = sms.selected == 1 ? 3 : sms.selected == 2 ? 4 : 1;
                                        }
#ifdef __SWITCH__
                                        padUpdate(&pad);
                                        lastHeld = padGetButtons(&pad);
#endif
                                    } else if (sms.selected == 3 && uu::monotonicMs() >= state.smsRetryAt) {
                                        client.command("sms-code", sms.credentials());
                                        sms.selected = 2;
                                    } else if (sms.selected == 4)
                                        client.command("sms-login", sms.credentials());
                                }
                            }
                        } else if (!busy && state.stage != "login") {
                            if (c.up)
                                selected = std::max(0, selected - 1);
                            if (c.down)
                                selected = std::min(std::max(0, int(state.devices.size()) - 1), selected + 1);
                            if (c.confirm) {
                                if (!state.authenticated)
                                    client.command("login");
                                else if (state.devices.empty())
                                    client.command("refresh");
                                else if (state.devices[selected].value("status", std::string{}) != "offline")
                                    client.command("connect",
                                                   {{"deviceId", state.devices[selected]["device_id"]},
                                                    {"fps", fps},
                                                    {"quality", quality},
                                                    {"customBps", bitrateSteps[bitrateIdx] * 1000000LL}});
                            }
                        }
                    }
                    lastHeld = c.held;
                    wasStreaming = state.streaming;
                    color(renderer, {13, 19, 25, 255});
                    SDL_RenderClear(renderer);
                    if (state.streaming) {
                        auto p = media->picture(serial);
                        if (p && p->serial != serial) {
                            if (p->width != texW || p->height != texH) {
                                if (video)
                                    SDL_DestroyTexture(video);
                                video = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32,
                                                          SDL_TEXTUREACCESS_STREAMING, p->width, p->height);
                                texW = p->width;
                                texH = p->height;
                            }
                            if (video)
                                SDL_UpdateTexture(video, nullptr, p->pixels.data(), p->width * 4);
                            serial = p->serial;
                            diagFrames++;
                            if (!diagLastMs)
                                diagLastMs = uu::monotonicMs();
                            else if (uu::monotonicMs() - diagLastMs >= 1000) {
                                diagFps = int(diagFrames * 1000 / (uu::monotonicMs() - diagLastMs));
                                diagFrames = 0;
                                diagLastMs = uu::monotonicMs();
                            }
                            float scale = std::min(1280.f / p->width, 720.f / p->height);
                            viewport = {int((1280 - p->width * scale) / 2), int((720 - p->height * scale) / 2),
                                        int(p->width * scale), int(p->height * scale)};
                        }
                        if (video)
                            SDL_RenderCopy(renderer, video, nullptr, &viewport);
                        else
                            text(renderer, fonts.normal, "等待电脑发送画面…", 470, 330, muted);
                        if (touchMode) {
                            // 触屏模式不显示鼠标箭头，触摸时画一个半透明指示圈
                            if (touchDotX >= 0 && touchDotY >= 0) {
                                for (int dy = -9; dy <= 9; ++dy) {
                                    const int half = int(std::sqrt(double(81 - dy * dy)));
                                    rect(renderer, touchDotX - half, touchDotY + dy, half * 2 + 1, 1,
                                         {255, 255, 255, 150});
                                }
                            }
                        } else {
                            int x = viewport.x + int(cursorX * viewport.w),
                                y = viewport.y + int(cursorY * viewport.h);
                            drawCursor(renderer, x, y);
                        }
                        if (overlay) {
                            rect(renderer, 0, 0, 1280, 68, {13, 19, 25, 225});
                            text(renderer, fonts.normal, "UU远程", 28, 18);
                            text(renderer, fonts.small,
                                 "左摇杆 鼠标  ·  ZR/ZL 点击  ·  A 确认  ·  B Esc  ·  - 断开  ·  + 隐藏帮助  ·  L3 HUD  ·  R3 触屏模式", 200,
                                 24, muted);
                            rect(renderer, 0, 650, 1280, 70, {13, 19, 25, 225});
                            text(renderer, fonts.small,
                                 "方向键 WASD  ·  X 空格  ·  Y Tab  ·  L Ctrl  ·  R Shift  ·  右摇杆 滚轮",
                                 28, 665, muted);
                            text(renderer, fonts.small, media->status(), 28, 694, muted);
                        }
                        std::string link;
                        if (state.pairType.find("relay") != std::string::npos)
                            link = " · 中继";
                        else if (state.pairType == "host/host")
                            link = " · 直连";
                        else if (!state.pairType.empty())
                            link = " · 穿透";
                        if (hudVisible)
                            text(renderer, fonts.small,
                                 std::to_string(texW) + "x" + std::to_string(texH) + " · " +
                                     std::to_string(diagFps) + "fps" + link +
                                         (touchMode ? " · 触屏" : ""),
                                 990, 694, accent);
                    } else {
                        text(renderer, fonts.large, "UU远程", 62, 44);
                        text(renderer, fonts.small, "NINTENDO SWITCH  v" UU_VERSION, 270, 69, accent);
                        {
                            std::lock_guard lk(devNoticeMutex);
                            if (!devNotice.empty())
                                text(renderer, fonts.small, devNotice, 270, 91, muted);
                        }
                        text(renderer, fonts.small, state.authenticated ? state.nickname : "尚未登录", 930,
                             69, muted, 280);
                        rect(renderer, 62, 127, 1156, 1, {44, 56, 67, 255});
                        if (state.qrUrl != qrUrl) {
                            qrUrl = state.qrUrl;
                            qr.reset();
                            if (!qrUrl.empty())
                                qr = std::make_unique<qrcodegen::QrCode>(qrcodegen::QrCode::encodeText(
                                    qrUrl.c_str(), qrcodegen::QrCode::Ecc::MEDIUM));
                        }
                        if (!state.authenticated && state.loginMode == "sms") {
                            text(renderer, fonts.large, "短信验证码登录", 62, 162);
                            text(renderer, fonts.small, "使用与电脑端相同的手机号", 62, 220, muted);
                            int xs[] = {80, 310, 840}, widths[] = {190, 490, 340};
                            const char *labels[] = {"国家 / 地区", "手机号码", "短信验证码"};
                            std::string values[] = {
                                "+" + sms.country, sms.mobile.empty() ? "输入手机号码" : sms.mobile,
                                sms.code.empty() ? "输入 6 位验证码" : std::string(sms.code.size(), '*')};
                            for (int field = 0; field < 3; ++field) {
                                text(renderer, fonts.small, labels[field], xs[field], 278, muted);
                                rect(renderer, xs[field], 316, widths[field], 64,
                                     sms.selected == field ? SDL_Color{32, 60, 57, 255}
                                                           : SDL_Color{28, 40, 49, 255});
                                if (sms.selected == field)
                                    rect(renderer, xs[field], 316, 4, 64, accent);
                                text(renderer, fonts.normal, values[field], xs[field] + 18, 334, white,
                                     widths[field] - 30);
                            }
                            int remaining = int(
                                std::max<int64_t>(0, (state.smsRetryAt - uu::monotonicMs() + 999) / 1000));
                            rect(renderer, 80, 424, 500, 68,
                                 sms.selected == 3 ? accent : SDL_Color{28, 40, 49, 255});
                            rect(renderer, 660, 424, 520, 68,
                                 sms.selected == 4 ? accent : SDL_Color{28, 40, 49, 255});
                            text(renderer, fonts.normal,
                                 remaining ? std::to_string(remaining) + " 秒后重发" : "发送验证码", 104, 444,
                                 sms.selected == 3 ? SDL_Color{13, 19, 25, 255} : white);
                            text(renderer, fonts.normal, "登录", 686, 444,
                                 sms.selected == 4 ? SDL_Color{13, 19, 25, 255} : white);
                            text(renderer, fonts.small, "↑ ↓ 选择项目   A 输入或确认   X 切换扫码登录", 80,
                                 514, muted);
                        } else if (state.stage == "login") {
                            text(renderer, fonts.large, "扫码登录", 62, 208);
                            text(renderer, fonts.normal, "使用手机 UU远程 App", 62, 294, white, 550);
                            text(renderer, fonts.normal, "扫描右侧二维码并确认", 62, 336, white, 550);
                            text(renderer, fonts.small, "X  短信验证码登录", 62, 432, accent);
                            text(renderer, fonts.small, "B  取消 / 返回", 62, 476, muted);
                            if (qr) {
                                auto layout = uu::qrGeometry(qr->getSize());
                                rect(renderer, layout.x, layout.y, layout.size, layout.size,
                                     {255, 255, 255, 255});
                                for (int y = 0; y < qr->getSize(); ++y)
                                    for (int x = 0; x < qr->getSize(); ++x)
                                        if (qr->getModule(x, y))
                                            rect(renderer, layout.x + (x + layout.border) * layout.scale,
                                                 layout.y + (y + layout.border) * layout.scale, layout.scale,
                                                 layout.scale, {0, 0, 0, 255});
                            } else
                                text(renderer, fonts.normal, "正在获取二维码…", 760, 360, muted);
                        } else {
                            text(renderer, fonts.small, "我的电脑", 62, 167, muted);
                            if (!state.devices.empty()) {
                                int first = std::max(0, selected - 3);
                                float target = 223.f + (selected - first) * 88;
                                focusY += (target - focusY) * std::min(1.f, dt * 18);
                                rect(renderer, 62, int(focusY), 535, 78, {32, 50, 53, 255});
                                rect(renderer, 62, int(focusY), 4, 78, accent);
                                for (int i = first; i < std::min(first + 4, int(state.devices.size())); ++i) {
                                    auto &device = state.devices[i];
                                    int y = 232 + (i - first) * 88;
                                    bool online = device.value("status", std::string{}) != "offline";
                                    text(renderer, fonts.normal, device.value("alias", std::string("电脑")),
                                         84, y, white, 470);
                                    text(renderer, fonts.small, online ? "在线" : "离线", 84, y + 35,
                                         online ? accent : muted);
                                }
                            } else {
                                text(renderer, fonts.normal,
                                     state.authenticated ? "暂未发现电脑" : "登录你的 UU 账号", 62, 237);
                                text(renderer, fonts.small,
                                     state.authenticated ? "请在电脑上登录同一账号"
                                                         : "扫码或短信登录，同步远程电脑",
                                     62, 287, muted);
                                rect(renderer, 62, 350, 440, 64, accent);
                                text(renderer, fonts.normal,
                                     state.authenticated ? "Y  刷新设备" : "A  扫码登录", 88, 367,
                                     {13, 19, 25, 255});
                                if (!state.authenticated)
                                    text(renderer, fonts.normal, "X  短信验证码登录", 62, 450, accent);
                            }
                            rect(renderer, 718, 237, 356, 214, {28, 40, 49, 255});
                            rect(renderer, 729, 248, 334, 192, {17, 25, 33, 255});
                            rect(renderer, 886, 451, 20, 45, {44, 56, 67, 255});
                            rect(renderer, 837, 495, 118, 6, {44, 56, 67, 255});
                            static const char *qualityNames[] = {"流畅", "高清", "超清", "原画", "自动",
                                                                 "自定义"};
                            bool custom = quality == 6;
                            text(renderer, fonts.small,
                                 std::string("L 画质  ") + qualityNames[quality - 1], 762, 364, white);
                            text(renderer, fonts.small, "R 帧率  " + std::to_string(fps) + " fps", 762, 386,
                                 white);
                            text(renderer, fonts.small,
                                 "ZR/ZL 码率  " + std::to_string(bitrateSteps[bitrateIdx]) + " Mbps",
                                 762, 408, custom ? white : muted);
                            text(renderer, fonts.small,
                                 custom ? "仅自定义画质档生效" : "切换后断开重连生效", 762, 430, muted);
                            text(renderer, fonts.small, "独立连接原版 UU 电脑端", 762, 535, muted);
                        }
                        if (busy || state.stage == "login") {
                            float pulse = (std::sin(now / 250.f) + 1) / 2;
                            rect(renderer, 62, 591, 6, 26,
                                 {accent.r, accent.g, accent.b, uint8_t(100 + pulse * 155)});
                        }
                        if (state.stage == "login")
                            text(renderer, fonts.small, state.detail, 82, 548, muted, 550);
                        else
                            statusText(renderer, fonts.small, state.detail,
                                       state.stage == "error" ? SDL_Color{255, 157, 141, 255} : muted);
                        rect(renderer, 62, 653, 1156, 1, {44, 56, 67, 255});
                        text(renderer, fonts.small,
                             state.authenticated
                                 ? "A 确认    B 返回    Y 刷新    X 退出账号    L 画质    R 帧率    ZR/ZL 码率"
                                 : "A 确认    B 返回    X 切换登录方式",
                             62, 675, muted);
                        text(renderer, fonts.small, "+ 退出", 1120, 675, muted);
                    }
                    SDL_RenderPresent(renderer);
                }
                devStop = true;
                if (devThread.joinable())
                    devThread.join();
                if (video)
                    SDL_DestroyTexture(video);
            }
            uu::waitBackgroundTasks();
            rtc::Cleanup().wait();
            media.reset();
            SDL_DestroyRenderer(renderer);
            SDL_DestroyWindow(window);
        }
        curl_global_cleanup();
        TTF_Quit();
        SDL_Quit();
    } catch (const std::exception &e) {
        result = 1;
        uu::diagnosticException("main-exception", std::current_exception());
        try {
            uu::waitBackgroundTasks();
            rtc::Cleanup().wait();
        } catch (...) {
            uu::diagnosticException("cleanup-exception", std::current_exception());
        }
#ifdef __SWITCH__
        SDL_Quit();
        consoleInit(nullptr);
        printf("UU Remote NS\n%s\nPress + to exit.\n", e.what());
        while (appletMainLoop()) {
            padUpdate(&pad);
            if (padGetButtonsDown(&pad) & HidNpadButton_Plus)
                break;
            consoleUpdate(nullptr);
        }
        consoleExit(nullptr);
#else
        std::cerr << e.what() << '\n';
#endif
    }
#ifdef __SWITCH__
    appletSetMediaPlaybackState(false);
    plExit();
    romfsExit();
    sslExit();
    nifmExit();
    socketExit();
#endif
    return result;
}
