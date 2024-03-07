#include "Entry.h"
#include "Global.h"
#include <filesystem>

#include <chrono>
#include <fmt/format.h>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>

#include "Config.h"

#include "httplib.h"
#include <WebSocketClient.h>
#include <nlohmann/json.hpp>
using namespace std;
using namespace cyanray;

using json = nlohmann::json;
ll::Logger logger(PLUGIN_NAME);
using namespace std;

using namespace ll::schedule;
using namespace ll::chrono_literals;


namespace change_this {

namespace {
Config config;
namespace fs = std::filesystem;
// ll3 事件注册，玩家进入游戏
ll::event::ListenerPtr playerJoinEventListener;

// ll3 事件注册，玩家退出游戏
ll::event::ListenerPtr player_life_game_Listener;

// 创建调度器的实例
ServerTimeScheduler s;

// ll3 事件注册，玩家退聊天
ll::event::ListenerPtr                                            player_chat_game_Listener;
WebSocketClient                                                   client;
std::unique_ptr<std::reference_wrapper<ll::plugin::NativePlugin>> selfPluginInstance;
auto                                                              disable(ll::plugin::NativePlugin& /*self*/) -> bool {
    auto& eventBus = ll::event::EventBus::getInstance();
    // 取消事件：玩家进入游戏
    eventBus.removeListener(playerJoinEventListener);
    // 取消事件：玩家退出游戏
    eventBus.removeListener(player_life_game_Listener);

    // 取消事件：玩家聊天
    eventBus.removeListener(player_chat_game_Listener);
    return true;
}
void periodicTask(string ws, string server_name) {

    const string ws_uri = ws + "/?server_name=" + server_name;
    try {
        client.Connect(ws_uri);
    } catch (const std::exception& ex) {
        cout << ex.what() << endl;
    }
    client.OnTextReceived([server_name = server_name](WebSocketClient& client, string text) {
        try {
            // 解析 JSON 字符串
            json jsonData = json::parse(text);

            // 获取 message 字段
            json messageData = jsonData["message"];

            // 获取 message 下的数据
            std::string data      = messageData["data"];
            std::string name      = messageData["name"];
            std::string server    = messageData["server"];
            const auto  chat_data = "§f[" + server + "]<" + name + "> " + data;
            if (server_name != server) {
                auto* level = GMLIB_Level::getLevel();
                level->broadcast(chat_data);
            }

        } catch (const std::exception& e) {
            // 处理解析异常
            cout << "JSON 解析错误: " << e.what() << endl;
        }
    });

    client.OnLostConnection([ws_uri](WebSocketClient& client, int code) {
        cout << "Lost connection: " << code << endl;
        while (true) {
            try {
                client.Connect(ws_uri);
                cout << "已重新连接." << endl;
                break;
            } catch (const std::exception& ex) {
                cout << ex.what() << endl;
            }
        }
    });
}

void get_players(string ip) {

    GMLIB::Server::FakeList::removeAllFakeLists();
    auto            level = ll::service::getLevel();
    httplib::Client cli(ip.c_str());
    auto            res = cli.Get("/api/game/pe/player/get");
    if (res && res->status == 200 && level.has_value()) {
        try {
            json                     players_data = json::parse(res->body);
            std::vector<std::string> all_player_names1;

            for (const auto& player_name : players_data) {
                all_player_names1.push_back(player_name);
            }

            std::vector<std::string> all_player_names2;
            level->forEachPlayer([&](Player& player) {
                std::string player_name = player.getName();
                all_player_names2.push_back(player_name);
                return true; // 返回 true 继续遍历，false 停止遍历
            });
            // 排序两个集合
            std::sort(all_player_names1.begin(), all_player_names1.end());
            std::sort(all_player_names2.begin(), all_player_names2.end());

            // 创建一个用于存储差异元素的新数组
            std::vector<std::string> difference;

            // 使用 set_difference 算法找到两个集合的差异元素
            std::set_difference(
                all_player_names1.begin(),
                all_player_names1.end(),
                all_player_names2.begin(),
                all_player_names2.end(),
                std::back_inserter(difference)
            );

            for (const auto& name : difference) {
                GMLIB::Server::FakeList::addFakeList(name, "002", ActorUniqueID());
            }
        } catch (const std::exception& e) {
            std::cerr << "JSON parsing error: " << e.what() << std::endl;
        }
    }
}
// /join
void http_post(string name, string server, string data, string post_url) {

    json jsonData = {
        {"name",   name  },
        {"server", server},
        {"data",   data  }
    };

    // 将JSON对象转换为字符串
    string jsonString = jsonData.dump();
    // HTTP
    httplib::Client  client(config.ip);
    httplib::Headers headers = {
        {"content-type", "application/json"}
    };
    auto response = client.Post("/api/game/pe/player" + post_url, headers, jsonString, "application/json");
}

auto enable(ll::plugin::NativePlugin& self) -> bool {
    auto& eventBus = ll::event::EventBus::getInstance();
    auto& logger   = self.getLogger();

    // 10秒一次
    s.add<RepeatTask>(5s, [&] { get_players(config.ip); });

    // 启动一个新线程，传入函数和参数
    std::thread myThread(periodicTask, config.ws, config.server_name);
    // 使用 detach 将新线程设置为后台线程
    myThread.detach();

    // 玩家已经进入服务器
    playerJoinEventListener = eventBus.emplaceListener<ll::event::player::PlayerJoinEvent>(
        [&logger, server_name = config.server_name, &self](ll::event::player::PlayerJoinEvent& event) {
            auto& player = event.self();
            http_post(player.getName(), server_name, "§e 玩家加入游戏", "/chat");
            http_post(player.getName(), server_name, "", "/join");
        }
    );

    // 玩家退出游戏
    player_life_game_Listener = eventBus.emplaceListener<ll::event::player::PlayerLeaveEvent>(
        [&logger, server_name = config.server_name, &self](ll::event::player::PlayerLeaveEvent& event) {
            auto& player = event.self();
            http_post(player.getName(), server_name, "§e 玩家退出游戏", "/chat");
            http_post(player.getName(), server_name, "", "/left");
        }
    );
    player_chat_game_Listener = eventBus.emplaceListener<ll::event::player::PlayerChatEvent>(
        [server_name = config.server_name](const ll::event::player::PlayerChatEvent& event) {
            // 当玩家聊天事件发生时执行的操作
            auto&      player  = event.self();
            const auto message = event.message(); // 调用 message() 函数来获取 std::string

            // 假设进行 HTTP POST 请求
            http_post(player.getName(), server_name, message, "/chat");
        }
    );
    return true;
}


auto load(ll::plugin::NativePlugin& self) -> bool {
    selfPluginInstance         = std::make_unique<std::reference_wrapper<ll::plugin::NativePlugin>>(self);
    const auto& configFilePath = self.getConfigDir() / "config.json";
    if (!ll::config::loadConfig(config, configFilePath)) {
        logger.warn("Cannot load configurations from {}", configFilePath);
        logger.info("Saving default configurations");

        if (!ll::config::saveConfig(config, configFilePath)) {
            logger.error("Cannot save default configurations to {}", configFilePath);
        }
    }
    return true;
}

auto unload(ll::plugin::NativePlugin& self) -> bool {
    auto& logger = self.getLogger();
    selfPluginInstance.reset();

    return true;
}

} // namespace

auto getSelfPluginInstance() -> ll::plugin::NativePlugin& {
    if (!selfPluginInstance) {
        throw std::runtime_error("selfPluginInstance is null");
    }
    return *selfPluginInstance;
}

} // namespace change_this

extern "C" {
_declspec(dllexport) auto ll_plugin_disable(ll::plugin::NativePlugin& self) -> bool {
    return change_this::disable(self);
}
_declspec(dllexport) auto ll_plugin_enable(ll::plugin::NativePlugin& self) -> bool { return change_this::enable(self); }
_declspec(dllexport) auto ll_plugin_load(ll::plugin::NativePlugin& self) -> bool { return change_this::load(self); }
_declspec(dllexport) auto ll_plugin_unload(ll::plugin::NativePlugin& self) -> bool { return change_this::unload(self); }
}
