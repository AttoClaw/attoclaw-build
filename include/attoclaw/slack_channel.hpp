#pragma once

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <filesystem>
#include <optional>

#include "attoclaw/channels.hpp"
#include "attoclaw/config.hpp"
#include "attoclaw/http.hpp"

namespace attoclaw {

class SlackChannel : public BaseChannel {
 public:
  SlackChannel(const SlackChannelConfig& config, MessageBus* bus) : BaseChannel("slack", bus), config_(config) {
    for (const auto& x : config_.allow_from) {
      allow_from_.insert(trim(x));
    }
    channels_ = config_.channels;
    state_path_ = expand_user_path("~/.attoclaw") / "state" / "slack_cursors.json";
  }

  void start() override {
    if (running_.exchange(true)) {
      return;
    }
    if (trim(config_.token).empty()) {
      Logger::log(Logger::Level::kWarn, "Slack enabled but token is empty; channel will not start.");
      running_.store(false);
      return;
    }
    if (channels_.empty()) {
      Logger::log(Logger::Level::kWarn, "Slack enabled but no channels configured; channel will not start.");
      running_.store(false);
      return;
    }

    load_state();
    worker_ = std::thread([this]() { poll_loop(); });
    Logger::log(Logger::Level::kInfo, "Slack channel started");
  }

  void stop() override {
    if (!running_.exchange(false)) {
      return;
    }
    if (worker_.joinable()) {
      worker_.join();
    }
    flush_state();
    Logger::log(Logger::Level::kInfo, "Slack channel stopped");
  }

  void send(const OutboundMessage& msg) override {
    if (trim(config_.token).empty()) {
      return;
    }
    thread_local HttpClient client;
    constexpr std::size_t kLimit = 38000;
    for (const auto& part : chunk_text(msg.content, kLimit)) {
      json payload = {{"channel", msg.chat_id}, {"text", part}};

      HttpResponse resp = client.post("https://slack.com/api/chat.postMessage", payload.dump(),
                                      {{"Authorization", "Bearer " + config_.token},
                                       {"Content-Type", "application/json"}},
                                      20, true, 3);
      if (!resp.error.empty()) {
        Logger::log(Logger::Level::kWarn, "Slack send failed: " + resp.error);
        return;
      }
      if (resp.status == 429) {
        const auto it = resp.headers.find("retry-after");
        const int wait_s = it == resp.headers.end() ? 3 : (std::max)(1, std::atoi(it->second.c_str()));
        std::this_thread::sleep_for(std::chrono::seconds(wait_s));
        continue;
      }
      if (resp.status < 200 || resp.status >= 300) {
        Logger::log(Logger::Level::kWarn, "Slack send failed: HTTP " + std::to_string(resp.status));
        return;
      }

      try {
        const json body = json::parse(resp.body);
        if (!body.value("ok", false)) {
          Logger::log(Logger::Level::kWarn, "Slack send failed: " + body.value("error", "unknown_error"));
        }
      } catch (...) {
      }
    }
  }

 private:
  void load_state() {
    const std::string raw = read_text_file(state_path_);
    if (trim(raw).empty()) {
      return;
    }
    try {
      const json j = json::parse(raw);
      if (!j.is_object()) {
        return;
      }
      if (j.contains("cursors") && j["cursors"].is_object()) {
        for (auto it = j["cursors"].begin(); it != j["cursors"].end(); ++it) {
          if (it.value().is_string()) {
            last_ts_[it.key()] = it.value().get<std::string>();
          }
        }
      }
    } catch (...) {
    }
  }

  void flush_state() {
    if (!dirty_.exchange(false)) {
      return;
    }
    json j;
    j["updatedAt"] = now_iso8601();
    j["cursors"] = json::object();
    for (const auto& kv : last_ts_) {
      j["cursors"][kv.first] = kv.second;
    }
    write_text_file(state_path_, j.dump(2));
    last_flush_ms_ = now_ms();
  }

  void maybe_flush_state() {
    const int64_t now = now_ms();
    if (!dirty_.load()) {
      return;
    }
    if (now - last_flush_ms_ < 2000) {
      return;
    }
    flush_state();
  }

  static std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
  }

  static bool looks_like_audio_file(const json& f) {
    if (!f.is_object()) {
      return false;
    }
    const std::string mimetype = f.value("mimetype", "");
    if (!mimetype.empty() && mimetype.rfind("audio/", 0) == 0) {
      return true;
    }
    const std::string filetype = f.value("filetype", "");
    const std::string name = f.value("name", "");
    const std::string combined = to_lower(filetype + " " + name);
    return combined.find("mp3") != std::string::npos || combined.find("m4a") != std::string::npos ||
           combined.find("wav") != std::string::npos || combined.find("ogg") != std::string::npos ||
           combined.find("opus") != std::string::npos;
  }

  std::optional<fs::path> download_slack_file(const std::string& url_private, const std::string& channel_id,
                                              const std::string& filename_hint) {
    if (trim(url_private).empty()) {
      return std::nullopt;
    }
    const fs::path base_dir = expand_user_path("~/.attoclaw") / "inbox" / "slack" / channel_id;
    std::error_code ec;
    fs::create_directories(base_dir, ec);
    fs::path out = base_dir / ("file_" + std::to_string(now_ms()));
    if (!trim(filename_hint).empty()) {
      out = base_dir / filename_hint;
    }

    HttpClient client;
    HttpResponse dl = client.download_to_file(url_private, {{"Authorization", "Bearer " + config_.token}}, out, 90, true, 3);
    if (!dl.error.empty() || dl.status < 200 || dl.status >= 300) {
      return std::nullopt;
    }
    return fs::absolute(out);
  }

  bool is_allowed_sender(const std::string& user_id) const {
    if (allow_from_.empty()) {
      return true;
    }
    return allow_from_.contains(user_id);
  }

  void poll_loop() {
    HttpClient client;
    const int poll_s = (std::max)(1, config_.poll_seconds);
    while (running_.load()) {
      for (const auto& channel_id : channels_) {
        if (!running_.load()) {
          break;
        }

        const bool warmup = (last_ts_.find(channel_id) == last_ts_.end());
        std::string oldest = "0";
        auto it = last_ts_.find(channel_id);
        if (it != last_ts_.end() && !it->second.empty()) {
          oldest = it->second;
        }

        const std::string url =
            "https://slack.com/api/conversations.history?limit=50&channel=" + channel_id + "&oldest=" + oldest;

        HttpResponse resp =
            client.get(url, {{"Authorization", "Bearer " + config_.token}}, 25, true, 2);
        if (!running_.load()) {
          break;
        }
        if (!resp.error.empty()) {
          Logger::log(Logger::Level::kWarn, "Slack poll error: " + resp.error);
          continue;
        }
        if (resp.status == 429) {
          const auto it = resp.headers.find("retry-after");
          const int wait_s = it == resp.headers.end() ? 3 : (std::max)(1, std::atoi(it->second.c_str()));
          Logger::log(Logger::Level::kWarn, "Slack rate limited. Sleeping " + std::to_string(wait_s) + "s");
          std::this_thread::sleep_for(std::chrono::seconds(wait_s));
          continue;
        }
        if (resp.status < 200 || resp.status >= 300) {
          Logger::log(Logger::Level::kWarn, "Slack poll HTTP error: " + std::to_string(resp.status));
          continue;
        }

        try {
          const json body = json::parse(resp.body);
          if (!body.value("ok", false) || !body.contains("messages") || !body["messages"].is_array()) {
            continue;
          }

          // Slack returns newest-first.
          const auto& msgs = body["messages"];
          if (warmup) {
            std::string max_ts;
            for (const auto& m : msgs) {
              if (m.is_object() && m.contains("ts") && m["ts"].is_string()) {
                const std::string ts = m["ts"].get<std::string>();
                if (max_ts.empty() || ts > max_ts) {
                  max_ts = ts;
                }
              }
            }
            if (!max_ts.empty()) {
              last_ts_[channel_id] = max_ts;
            }
            continue;  // Do not replay history on first start.
          }

          for (auto it_msg = msgs.rbegin(); it_msg != msgs.rend(); ++it_msg) {
            const json& m = *it_msg;
            if (!m.is_object()) {
              continue;
            }
            if (m.contains("subtype") && m["subtype"].is_string()) {
              const std::string subtype = m["subtype"].get<std::string>();
              if (subtype == "bot_message" || subtype == "message_changed" || subtype == "message_deleted") {
                continue;
              }
            }
            if (!m.contains("user") || !m["user"].is_string()) {
              continue;
            }
            if (!m.contains("text") || !m["text"].is_string()) {
              continue;
            }
            if (!m.contains("ts") || !m["ts"].is_string()) {
              continue;
            }
            const std::string user_id = m["user"].get<std::string>();
            if (!is_allowed_sender(user_id)) {
              continue;
            }
            std::string text = trim(m["text"].get<std::string>());
            const std::string ts = m["ts"].get<std::string>();
            if (last_ts_[channel_id].empty() || ts > last_ts_[channel_id]) {
              last_ts_[channel_id] = ts;
              dirty_.store(true);
            }

            std::vector<std::string> media_paths;
            if (m.contains("files") && m["files"].is_array()) {
              for (const auto& f : m["files"]) {
                if (!looks_like_audio_file(f)) {
                  continue;
                }
                const std::string url_private = f.value("url_private_download", f.value("url_private", ""));
                const std::string name = f.value("name", "");
                if (auto p = download_slack_file(url_private, channel_id, name)) {
                  media_paths.push_back(p->string());
                  break;
                }
              }
            }

            if (text.empty() && !media_paths.empty()) {
              text = "Voice/audio file received. Please transcribe and respond.";
            }
            if (text.empty() && media_paths.empty()) {
              continue;
            }

            handle_message(user_id, channel_id, text, media_paths, json::object());
          }
        } catch (const std::exception& e) {
          Logger::log(Logger::Level::kWarn, std::string("Slack parse error: ") + e.what());
        }

        maybe_flush_state();
      }

      for (int i = 0; running_.load() && i < poll_s * 10; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
    }
  }

  SlackChannelConfig config_;
  std::vector<std::string> channels_;
  std::unordered_set<std::string> allow_from_;
  std::unordered_map<std::string, std::string> last_ts_;
  fs::path state_path_;
  std::atomic<bool> dirty_{false};
  int64_t last_flush_ms_{0};

  std::atomic<bool> running_{false};
  std::thread worker_;
};

}  // namespace attoclaw
