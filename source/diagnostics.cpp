#include "diagnostics.hpp"
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <mutex>
#include <new>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#ifdef __SWITCH__
#include <switch.h>
#include <unwind.h>
extern "C" char __start__[];
#endif

namespace uu {
const char *diagnosticPath() noexcept {
#ifdef __SWITCH__
    return "sdmc:/switch/UURemote-ns/connection.log";
#else
    return "connection.log";
#endif
}
namespace {
// 日志写入必须在单独线程：此前在 RTP 接收线程里同步 fopen/fflush/fclose
// （Switch 的 SD 卡一次可能几十毫秒）会阻塞收包，造成 UDP 缓冲区溢出丢包。
std::mutex gLogMutex;
std::condition_variable gLogCv;
std::deque<std::string> gLogQueue;
bool gLogStop = false;
std::thread gLogThread;
void writeBatch(const std::deque<std::string> &lines) {
    if (lines.empty())
        return;
    auto *file = std::fopen(diagnosticPath(), "a");
    if (!file)
        return;
    std::fseek(file, 0, SEEK_END);
    long size = std::ftell(file);
    // 滚动阈值留足余量：预算耗尽时必须重置，否则文件卡在上限后再也写不进任何行。
    if (size > 96 * 1024) {
        std::fclose(file);
        file = std::fopen(diagnosticPath(), "w");
        if (!file)
            return;
        size = 0;
    }
    size_t budget = size_t(128 * 1024 - size);
    for (const auto &line : lines) {
        const size_t need = line.size() + 1;
        if (need > budget)
            break;
        budget -= need;
        std::fprintf(file, "%s\n", line.c_str());
    }
    std::fflush(file);
    std::fclose(file);
}
void logWriter() {
    for (;;) {
        std::deque<std::string> batch;
        {
            std::unique_lock lock(gLogMutex);
            gLogCv.wait(lock, [] { return gLogStop || !gLogQueue.empty(); });
            if (gLogQueue.empty())
                return;
            while (!gLogQueue.empty() && batch.size() < 128) {
                batch.push_back(std::move(gLogQueue.front()));
                gLogQueue.pop_front();
            }
        }
        writeBatch(batch);
    }
}
void ensureLogThread() {
    static std::once_flag once;
    std::call_once(once, [] { gLogThread = std::thread(logWriter); });
}
} // namespace
void diagnostic(const char *event) noexcept {
    // Fixed names, numeric error codes and code offsets only; never log protocol data.
    try {
        ensureLogThread();
        char prefix[24];
        std::snprintf(prefix, sizeof prefix, "%lld ", static_cast<long long>(std::time(nullptr)));
        std::lock_guard lock(gLogMutex);
        if (gLogQueue.size() >= 512)
            return; // 拥塞时丢弃，绝不阻塞调用方
        gLogQueue.emplace_back(std::string(prefix) + event);
        gLogCv.notify_one();
    } catch (...) {
    }
}
std::string exceptionSummary(std::exception_ptr error) {
    if (!error)
        return "exception-unavailable";
    try {
        std::rethrow_exception(error);
    } catch (const std::system_error &e) {
        return "exception-system-error-" + std::to_string(e.code().value());
    } catch (const std::bad_alloc &) {
        return "exception-out-of-memory";
    } catch (const std::logic_error &) {
        return "exception-logic-error";
    } catch (const std::runtime_error &) {
        return "exception-runtime-error";
    } catch (const std::exception &) {
        return "exception-std";
    } catch (...) {
        return "exception-unknown";
    }
}
void diagnosticException(const char *event, std::exception_ptr error) noexcept {
    diagnostic(event);
    try {
        diagnostic(exceptionSummary(error).c_str());
    } catch (...) {
        diagnostic("exception-summary-unavailable");
    }
}
void flushDiagnostics() noexcept {
    try {
        std::deque<std::string> batch;
        {
            std::lock_guard lock(gLogMutex);
            batch.swap(gLogQueue);
        }
        writeBatch(batch);
    } catch (...) {
    }
}
[[noreturn]] void terminateWithDiagnostics() noexcept {
    diagnosticException("runtime-terminate", std::current_exception());
#ifdef __SWITCH__
    int depth = 0;
    _Unwind_Backtrace(
        [](_Unwind_Context *context, void *argument) {
            auto &depth = *static_cast<int *>(argument);
            const uintptr_t ip = _Unwind_GetIP(context), base = reinterpret_cast<uintptr_t>(__start__);
            if (ip >= base && ip - base < 256 * 1024 * 1024) {
                char line[64];
                std::snprintf(line, sizeof(line), "trace-%02d-0x%llx", depth,
                              static_cast<unsigned long long>(ip - base));
                diagnostic(line);
            }
            return ++depth < 16 ? _URC_NO_REASON : _URC_END_OF_STACK;
        },
        &depth);
    // libc abort/exit can tear down the shared window while the UI thread is presenting.
    // Exit the process directly after saving evidence, without running global destructors.
    flushDiagnostics();
    svcExitProcess();
#else
    std::abort();
#endif
}
std::string connectionFailure(const std::string &stage, const std::string &error) {
    std::string name;
    if (stage == "joining")
        name = "加入房间";
    else if (stage == "signaling")
        name = "信令连接";
    else if (stage == "negotiating")
        name = "申请控制权";
    else if (stage == "rtc-init")
        name = "初始化串流";
    else if (stage == "connecting")
        name = "音视频协商";
    else if (stage == "connected")
        name = "远程会话";
    return name.empty() ? error : name + "失败：" + error;
}
} // namespace uu
