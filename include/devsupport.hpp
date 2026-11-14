#pragma once
#include <string>
#include <vector>

namespace uu {
// 读取 SD 卡 devserver.txt（每行 host:port），返回 http://host:port 候选列表。
std::vector<std::string> loadDevServers();

// 一轮开发支持：向服务器上报 connection.log、检查新版本并自更新 NRO。
// 返回新版本号（空 = 本次无更新/不可用）。
std::string devTick(const std::vector<std::string> &bases, const std::string &logPath,
                    const std::string &nroPath);
} // namespace uu
