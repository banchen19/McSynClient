#pragma once

#include <string>
namespace change_this {

struct Config {
    int  version                = 1;
    std::string  server_name                = "BDS端";
    std::string  ws                = "ws://127.0.0.1:2000";
    std::string  ip                = "http://127.0.0.1:2000";
    bool doGiveClockOnFirstJoin = true;
    bool enableClockMenu        = true;
};

} // namespace better_suicide