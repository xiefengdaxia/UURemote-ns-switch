#include "devsupport.hpp"
#include "diagnostics.hpp"
#include "protocol.hpp"
#include <curl/curl.h>
#include <cstdio>
#include <fstream>
#include <iterator>
#ifdef __SWITCH__
#include <dirent.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <switch.h>
#endif

namespace uu {
namespace {
bool httpCall(const std::string &url, bool post, const std::string &body, long timeoutMs,
              std::string &response, long &status, const std::string &headerName,
              const std::string &headerValue);
size_t writeAppend(char *ptr, size_t size, size_t nmemb, void *userdata) {
    auto *out = static_cast<std::string *>(userdata);
    out->append(ptr, size * nmemb);
    return size * nmemb;
}

bool httpCall(const std::string &url, bool post, const std::string &body, long timeoutMs,
              std::string &response, long &status) {
    return httpCall(url, post, body, timeoutMs, response, status, {}, {});
}
bool httpCall(const std::string &url, bool post, const std::string &body, long timeoutMs,
              std::string &response, long &status, const std::string &headerName,
              const std::string &headerValue) {
    CURL *c = curl_easy_init();
    if (!c)
        return false;
    response.clear();
    status = 0;
    curl_slist *headers = nullptr;
    if (!headerName.empty())
        headers = curl_slist_append(headers, (headerName + ": " + headerValue).c_str());
    if (headers)
        curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, writeAppend);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(c, CURLOPT_TIMEOUT_MS, timeoutMs);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT_MS, 1500);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    if (post) {
        curl_easy_setopt(c, CURLOPT_POSTFIELDS, body.c_str());
        curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, long(body.size()));
    }
    bool ok = curl_easy_perform(c) == CURLE_OK;
    if (ok)
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(c);
    if (headers)
        curl_slist_free_all(headers);
    return ok;
}
} // namespace

std::vector<std::string> loadDevServers() {
    std::vector<std::string> bases;
#ifdef __SWITCH__
    std::ifstream f("sdmc:/switch/UURemote-ns/devserver.txt");
#else
    std::ifstream f("devserver.txt");
#endif
    std::string line;
    while (std::getline(f, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
            line.pop_back();
        if (!line.empty())
            bases.push_back("http://" + line);
    }
    return bases;
}

std::string devTick(const std::vector<std::string> &bases, const std::string &logPath,
                    const std::string &nroPath) {
#ifdef __SWITCH__
    static bool dirLogged = false;
    if (!dirLogged) {
        dirLogged = true;
        DIR *d = opendir("sdmc:/switch/UURemote-ns");
        if (d) {
            struct dirent *e;
            while ((e = readdir(d)) != nullptr) {
                struct stat st {};
                std::string full = std::string("sdmc:/switch/UURemote-ns/") + e->d_name;
                stat(full.c_str(), &st);
                std::string name = e->d_name;
                for (auto &ch : name)
                    if (ch == ' ')
                        ch = '_';
                diagnostic(("dir-" + name + "-" + std::to_string(st.st_size)).c_str());
            }
            closedir(d);
        } else {
            diagnostic("dir-open-fail");
        }
    }
#endif
    static std::string working;
    if (working.empty()) {
        for (const auto &b : bases) {
            long status = 0;
            std::string response;
            if (httpCall(b + "/health", false, "", 2500, response, status) && status == 200) {
                working = b;
                break;
            }
        }
        if (working.empty())
            return {};
    }
#ifdef __SWITCH__
    // 探测 Horizon 允许的 UDP 接收缓冲上限：视频突发丢失可能源于此处。
    static bool bufProbed = false;
    if (!bufProbed) {
        bufProbed = true;
        const int fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (fd >= 0) {
            int want = 4 << 20;
            setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &want, sizeof want);
            int got = 0;
            socklen_t len = sizeof got;
            getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &got, &len);
            char line[48];
            std::snprintf(line, sizeof line, "sockbuf-rcv-%d", got);
            diagnostic(line);
            close(fd);
        }
    }
    // 链路吞吐实测：每次启动下载 4MB，判断 Wi-Fi 是不是码率瓶颈。
    static bool speedTested = false;
    if (!speedTested) {
        speedTested = true;
        long status = 0;
        std::string payload;
        const auto start = monotonicMs();
        if (httpCall(working + "/speedtest", false, "", 60000, payload, status) && status == 200 &&
            payload.size() > (1u << 20)) {
            const auto elapsed = monotonicMs() - start;
            const unsigned kbps =
                elapsed > 0 ? unsigned(double(payload.size()) * 8.0 / double(elapsed)) : 0;
            char line[80];
            std::snprintf(line, sizeof line, "net-dl-%llukb-%llums-%ukbps",
                          static_cast<unsigned long long>(payload.size() / 1024),
                          static_cast<unsigned long long>(elapsed), kbps);
            diagnostic(line);
            // 同步 POST 给服务器，避免异步日志写入与上传抢时序导致结果丢失
            httpCall(working + "/speed", true, line, 5000, payload, status);
        } else {
            diagnostic("net-dl-fail");
            httpCall(working + "/speed", true, "net-dl-fail", 5000, payload, status);
        }
    }
#endif
    {
        // 上报诊断日志
        std::ifstream f(logPath, std::ios::binary);
        std::string body(std::istreambuf_iterator<char>(f), {});
        long status = 0;
        std::string response;
        httpCall(working + "/log", true, body, 5000, response, status, "X-App-Version", UU_VERSION);
    }
#ifdef __SWITCH__
    // 检查新版本并自更新（仅真机；桌面构建不覆盖自身）
    long status = 0;
    std::string version;
    if (!httpCall(working + "/version", false, "", 2500, version, status) || status != 200)
        return {};
    while (!version.empty() && (version.back() == '\r' || version.back() == '\n' ||
                                version.back() == ' '))
        version.pop_back();
    if (version.empty() || version == UU_VERSION)
        return {};
    std::string nro;
    if (!httpCall(working + "/build.nro", false, "", 60000, nro, status) || status != 200) {
        diagnostic("su-dl-fail");
        return {};
    }
    diagnostic("su-dl-ok");
    if (nro.size() < (1u << 20) || nro.size() > (64u << 20)) {
        diagnostic("su-size-fail");
        return {};
    }
    if (nro.compare(16, 4, "NRO0") != 0) {
        diagnostic("su-magic-fail");
        return {};
    }
    auto writeOut = [&](const std::string &path) {
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        f.write(nro.data(), std::streamsize(nro.size()));
        return bool(f);
    };
    auto errnoOf = []() { return std::string("-e") + std::to_string(errno); };
    // 路径一：写临时文件 -> 删除旧包 -> 改名替换（多手段，记录 errno）
    const std::string newPath = nroPath + ".new";
    if (writeOut(newPath)) {
        diagnostic("su-write-ok");
#ifdef __SWITCH__
        FsFileSystem *sd = fsdevGetDeviceFileSystem("sdmc");
        if (sd) {
            Result r = fsFsDeleteFile(sd, "switch/UURemote-ns/UURemote-ns.nro");
            diagnostic(("su-fsdev-del-" + std::to_string(r)).c_str());
        }
#endif
        errno = 0;
        int rm = std::remove(nroPath.c_str());
        diagnostic(("su-remove-" + std::to_string(rm) + errnoOf()).c_str());
        errno = 0;
        int ul = unlink(nroPath.c_str());
        diagnostic(("su-unlink-" + std::to_string(ul) + errnoOf()).c_str());
#ifdef __SWITCH__
        if (sd) {
            Result r = fsFsRenameFile(sd, "switch/UURemote-ns/UURemote-ns.nro.new",
                                      "switch/UURemote-ns/UURemote-ns.nro");
            diagnostic(("su-fsdev-ren-" + std::to_string(r)).c_str());
            if (R_SUCCEEDED(r))
                return version;
        }
#endif
        errno = 0;
        if (std::rename(newPath.c_str(), nroPath.c_str()) == 0)
            return version;
        diagnostic(("su-rename" + errnoOf()).c_str());
    } else {
        diagnostic(("su-write-new" + errnoOf()).c_str());
    }
    errno = 0;
    if (writeOut(nroPath)) {
        diagnostic(("su-direct-ok" + errnoOf()).c_str());
        return version;
    }
    // 兜底：写成独立入口文件，用户在 hbmenu 里启动它完成换装
    const std::string altPath =
        nroPath.substr(0, nroPath.find_last_of('.')) + ".update.nro";
    if (writeOut(altPath)) {
        diagnostic("su-alt-ok");
        return "update:" + version;
    }
    diagnostic(("su-alt-fail" + errnoOf()).c_str());
    return {};
#else
    return {};
#endif
}
} // namespace uu
