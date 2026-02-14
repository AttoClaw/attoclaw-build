#pragma once

#include <atomic>
#include <string>
#include <thread>
#include <unordered_set>

#include "attoclaw/channels.hpp"
#include "attoclaw/config.hpp"
#include "attoclaw/http.hpp"

namespace attoclaw {

class TelegramChannel : public BaseChannel {
 public:
  TelegramChannel(const TelegramChannelConfig& config, MessageBus* bus)
      : BaseChannel("telegram", bus), config_(config), token_(config.token) {
    for (const auto& x : config_.allow_from) {
      allow_from_.insert(trim(x));
    }
  }

  void start() override {
    if (running_.exchange(true)) {
      return;
    }
    if (trim(token_).empty()) {
      Logger::log(Logger::Level::kWarn, "Telegram enabled but token is empty; channel will not start.");
      running_.store(false);
      return;
    }

    worker_ = std::thread([this]() { poll_loop(); });
    Logger::log(Logger::Level::kInfo, "Telegram channel started");
  }

  void stop() override {
    if (!running_.exchange(false)) {
      return;
    }
    if (worker_.joinable()) {
      worker_.join();
    }
    Logger::log(Logger::Level::kInfo, "Telegram channel stopped");
  }

  void send(const OutboundMessage& msg) override {
    if (trim(token_).empty()) {
      return;
    }
    HttpClient client;
    json payload = {
        {"chat_id", msg.chat_id},
        {"text", msg.content},
    };
    const std::string url = api_base() + "/sendMessage";
    HttpResponse resp =
        client.post(url, payload.dump(), {{"Content-Type", "application/json"}}, 15, true, 3);
    if (!resp.error.empty() || resp.status < 200 || resp.status >= 300) {
      Logger::log(Logger::Level::kWarn,
                  "Telegram send failed: " +
                      (!resp.error.empty() ? resp.error : ("HTTP " + std::to_string(resp.status))));
    }
  }

 private:
  static std::string json_to_string(const json& v) {
    if (v.is_string()) {
      return v.get<std::string>();
    }
    if (v.is_number_integer()) {
      return std::to_string(v.get<long long>());
    }
    if (v.is_number_unsigned()) {
      return std::to_string(v.get<unsigned long long>());
    }
    if (v.is_number_float()) {
      std::ostringstream ss;
      ss << v.get<double>();
      return ss.str();
    }
    return "";
  }

  bool is_allowed_sender(const json& from_obj) const {
    if (allow_from_.empty()) {
      return true;
    }
    std::vector<std::string> candidates;
    if (from_obj.contains("id")) {
      candidates.push_back(json_to_string(from_obj["id"]));
    }
    if (from_obj.contains("username") && from_obj["username"].is_string()) {
      const std::string u = from_obj["username"].get<std::string>();
      candidates.push_back(u);
      candidates.push_back("@" + u);
    }
    for (const auto& c : candidates) {
      if (allow_from_.contains(c)) {
        return true;
      }
    }
    return false;
  }

  void process_update(const json& update) {
    if (!update.contains("message") || !update["message"].is_object()) {
      return;
    }
    const json& message = update["message"];
    if (!message.contains("from") || !message["from"].is_object()) {
      return;
    }
    const json& from = message["from"];
    if (from.value("is_bot", false)) {
      return;
    }
    if (!is_allowed_sender(from)) {
      return;
    }

    std::string content;
    if (message.contains("text") && message["text"].is_string()) {
      content = message["text"].get<std::string>();
    } else if (message.contains("caption") && message["caption"].is_string()) {
      content = message["caption"].get<std::string>();
    }
    if (trim(content).empty()) {
      return;
    }

    std::string sender_id = from.contains("id") ? json_to_string(from["id"]) : "";
    std::string chat_id;
    if (message.contains("chat") && message["chat"].is_object() && message["chat"].contains("id")) {
      chat_id = json_to_string(message["chat"]["id"]);
    }
    if (sender_id.empty() || chat_id.empty()) {
      return;
    }

    handle_message(sender_id, chat_id, content);
  }

  void poll_loop() {
    HttpClient client;
    while (running_.load()) {
      const std::string url =
          api_base() + "/getUpdates?timeout=20&offset=" + std::to_string(next_update_offset_) +
          "&allowed_updates=%5B%22message%22%5D";

      HttpResponse resp = client.get(url, {}, 25, true, 2);
      if (!running_.load()) {
        break;
      }
      if (!resp.error.empty()) {
        Logger::log(Logger::Level::kWarn, "Telegram getUpdates error: " + resp.error);
        std::this_thread::sleep_for(std::chrono::seconds(2));
        continue;
      }
      if (resp.status < 200 || resp.status >= 300) {
        Logger::log(Logger::Level::kWarn,
                    "Telegram getUpdates HTTP error: " + std::to_string(resp.status));
        std::this_thread::sleep_for(std::chrono::seconds(2));
        continue;
      }

      try {
        const json body = json::parse(resp.body);
        if (!body.value("ok", false) || !body.contains("result") || !body["result"].is_array()) {
          continue;
        }
        for (const auto& update : body["result"]) {
          if (update.contains("update_id") && update["update_id"].is_number_integer()) {
            const long long update_id = update["update_id"].get<long long>();
            if (update_id + 1 > next_update_offset_) {
              next_update_offset_ = update_id + 1;
            }
          }
          process_update(update);
        }
      } catch (const std::exception& e) {
        Logger::log(Logger::Level::kWarn, std::string("Telegram parse error: ") + e.what());
      }
    }
  }

  std::string api_base() const { return "https://api.telegram.org/bot" + token_; }

  TelegramChannelConfig config_;
  std::string token_;
  std::unordered_set<std::string> allow_from_;
  std::atomic<bool> running_{false};
  std::thread worker_;
  long long next_update_offset_{0};
};

}  // namespace attoclaw


