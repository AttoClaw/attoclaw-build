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

class DiscordChannel : public BaseChannel {
 public:
  DiscordChannel(const DiscordChannelConfig& config, MessageBus* bus)
      : BaseChannel("discord", bus), config_(config), api_base_(trim(config.api_base).empty() ? "https://discord.com/api/v10" : trim(config.api_base)) {
    for (const auto& x : config_.allow_from) {
      allow_from_.insert(trim(x));
    }
    channels_ = config_.channels;
    state_path_ = expand_user_path("~/.attoclaw") / "state" / "discord_cursors.json";
  }

  void start() override {
    if (running_.exchange(true)) {
      return;
    }
    if (trim(config_.token).empty()) {
      Logger::log(Logger::Level::kWarn, "Discord enabled but token is empty; channel will not start.");
      running_.store(false);
      return;
    }
    if (channels_.empty()) {
      Logger::log(Logger::Level::kWarn, "Discord enabled but no channels configured; channel will not start.");
      running_.store(false);
      return;
    }

    load_state();
    worker_ = std::thread([this]() { poll_loop(); });
    Logger::log(Logger::Level::kInfo, "Discord channel started");
  }

  void stop() override {
    if (!running_.exchange(false)) {
      return;
    }
    if (worker_.joinable()) {
      worker_.join();
    }
    flush_state();
    Logger::log(Logger::Level::kInfo, "Discord channel stopped");
  }

  void send(const OutboundMessage& msg) override {
    if (trim(config_.token).empty()) {
      return;
    }
    thread_local HttpClient client;
    constexpr std::size_t kLimit = 1900;
    const std::string url = api_base_ + "/channels/" + msg.chat_id + "/messages";
    for (const auto& part : chunk_text(msg.content, kLimit)) {
      json payload = {{"content", part}};
      HttpResponse resp = client.post(url, payload.dump(),
                                      {{"Authorization", "Bot " + config_.token},
                                       {"Content-Type", "application/json"}},
                                      20, true, 3);
      if (resp.status == 429) {
        const auto it = resp.headers.find("retry-after");
        const int wait_s = it == resp.headers.end() ? 3 : (std::max)(1, std::atoi(it->second.c_str()));
        std::this_thread::sleep_for(std::chrono::seconds(wait_s));
        continue;
      }
      if (!resp.error.empty() || resp.status < 200 || resp.status >= 300) {
        Logger::log(Logger::Level::kWarn, "Discord send failed: " +
                                          (!resp.error.empty() ? resp.error : ("HTTP " + std::to_string(resp.status))));
        break;
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
            last_id_[it.key()] = it.value().get<std::string>();
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
    for (const auto& kv : last_id_) {
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

  static bool looks_like_audio_attachment(const json& a) {
    if (!a.is_object()) {
      return false;
    }
    if (a.contains("content_type") && a["content_type"].is_string()) {
      const std::string ct = a["content_type"].get<std::string>();
      if (!ct.empty() && ct.rfind("audio/", 0) == 0) {
        return true;
      }
    }
    const std::string fn = to_lower(a.value("filename", ""));
    return fn.find(".mp3") != std::string::npos || fn.find(".m4a") != std::string::npos ||
           fn.find(".wav") != std::string::npos || fn.find(".ogg") != std::string::npos ||
           fn.find(".opus") != std::string::npos;
  }

  std::optional<fs::path> download_discord_attachment(const std::string& url, const std::string& channel_id,
                                                      const std::string& filename_hint) {
    if (trim(url).empty()) {
      return std::nullopt;
    }
    const fs::path base_dir = expand_user_path("~/.attoclaw") / "inbox" / "discord" / channel_id;
    std::error_code ec;
    fs::create_directories(base_dir, ec);
    fs::path out = base_dir / ("file_" + std::to_string(now_ms()));
    if (!trim(filename_hint).empty()) {
      out = base_dir / filename_hint;
    }

    HttpClient client;
    HttpResponse dl = client.download_to_file(url, {}, out, 90, true, 3);
    if (!dl.error.empty() || dl.status < 200 || dl.status >= 300) {
      return std::nullopt;
    }
    return fs::absolute(out);
  }

  static std::optional<unsigned long long> parse_snowflake(const std::string& id) {
    try {
      return std::stoull(id);
    } catch (...) {
      return std::nullopt;
    }
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

        std::string url = api_base_ + "/channels/" + channel_id + "/messages?limit=50";
        const bool warmup = (last_id_.find(channel_id) == last_id_.end());
        auto it = last_id_.find(channel_id);
        if (it != last_id_.end() && !it->second.empty()) {
          url += "&after=" + it->second;
        }

        HttpResponse resp =
            client.get(url, {{"Authorization", "Bot " + config_.token}}, 25, true, 2);
        if (!running_.load()) {
          break;
        }
        if (!resp.error.empty()) {
          Logger::log(Logger::Level::kWarn, "Discord poll error: " + resp.error);
          continue;
        }
        if (resp.status == 429) {
          const auto it_ra = resp.headers.find("retry-after");
          const int wait_s = it_ra == resp.headers.end() ? 3 : (std::max)(1, std::atoi(it_ra->second.c_str()));
          Logger::log(Logger::Level::kWarn, "Discord rate limited. Sleeping " + std::to_string(wait_s) + "s");
          std::this_thread::sleep_for(std::chrono::seconds(wait_s));
          continue;
        }
        if (resp.status < 200 || resp.status >= 300) {
          Logger::log(Logger::Level::kWarn, "Discord poll HTTP error: " + std::to_string(resp.status));
          continue;
        }

        try {
          const json arr = json::parse(resp.body);
          if (!arr.is_array()) {
            continue;
          }

          // Discord returns newest-first. Iterate oldest-first.
          unsigned long long max_seen = 0;
          if (it != last_id_.end() && !it->second.empty()) {
            if (auto v = parse_snowflake(it->second)) {
              max_seen = *v;
            }
          }

          if (warmup) {
            for (const auto& m : arr) {
              if (!m.is_object() || !m.contains("id") || !m["id"].is_string()) {
                continue;
              }
              if (auto snow = parse_snowflake(m["id"].get<std::string>())) {
                if (*snow > max_seen) {
                  max_seen = *snow;
                }
              }
            }
            if (max_seen != 0) {
              last_id_[channel_id] = std::to_string(max_seen);
            }
            continue;  // Do not replay history on first start.
          }

          for (auto it_msg = arr.rbegin(); it_msg != arr.rend(); ++it_msg) {
            const json& m = *it_msg;
            if (!m.is_object()) {
              continue;
            }
            if (!m.contains("id") || !m["id"].is_string()) {
              continue;
            }
            if (!m.contains("content") || !m["content"].is_string()) {
              continue;
            }
            if (!m.contains("author") || !m["author"].is_object()) {
              continue;
            }
            const json author = m["author"];
            if (author.value("bot", false)) {
              continue;
            }
            const std::string user_id = author.value("id", "");
            if (user_id.empty() || !is_allowed_sender(user_id)) {
              continue;
            }

            const std::string msg_id = m["id"].get<std::string>();
            const auto snow = parse_snowflake(msg_id);
            if (!snow.has_value()) {
              continue;
            }
            if (*snow > max_seen) {
              max_seen = *snow;
            }

            std::string text = trim(m["content"].get<std::string>());
            std::vector<std::string> media_paths;
            if (m.contains("attachments") && m["attachments"].is_array()) {
              for (const auto& a : m["attachments"]) {
                if (!looks_like_audio_attachment(a)) {
                  continue;
                }
                const std::string url = a.value("url", "");
                const std::string fn = a.value("filename", "");
                if (auto p = download_discord_attachment(url, channel_id, fn)) {
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

          if (max_seen != 0) {
            last_id_[channel_id] = std::to_string(max_seen);
            dirty_.store(true);
          }
        } catch (const std::exception& e) {
          Logger::log(Logger::Level::kWarn, std::string("Discord parse error: ") + e.what());
        }

        maybe_flush_state();
      }

      for (int i = 0; running_.load() && i < poll_s * 10; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
    }
  }

  DiscordChannelConfig config_;
  std::string api_base_;
  std::vector<std::string> channels_;
  std::unordered_set<std::string> allow_from_;
  std::unordered_map<std::string, std::string> last_id_;
  fs::path state_path_;
  std::atomic<bool> dirty_{false};
  int64_t last_flush_ms_{0};

  std::atomic<bool> running_{false};
  std::thread worker_;
};

}  // namespace attoclaw
