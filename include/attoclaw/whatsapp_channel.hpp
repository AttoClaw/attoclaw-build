#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include <curl/curl.h>
#include <curl/websockets.h>

#include "attoclaw/channels.hpp"
#include "attoclaw/config.hpp"

namespace attoclaw {

class WhatsAppChannel : public BaseChannel {
 public:
  WhatsAppChannel(const WhatsAppChannelConfig& config, MessageBus* bus)
      : BaseChannel("whatsapp", bus),
        config_(config),
        bridge_url_(trim(config.bridge_url)),
        bridge_token_(trim(config.bridge_token)) {
    for (const auto& x : config_.allow_from) {
      const std::string t = trim(x);
      if (!t.empty()) {
        allow_from_.insert(t);
      }
    }
  }

  void start() override {
    if (running_.exchange(true)) {
      return;
    }
    if (bridge_url_.empty()) {
      Logger::log(Logger::Level::kWarn,
                  "WhatsApp enabled but bridgeUrl is empty; channel will not start.");
      running_.store(false);
      return;
    }

    worker_ = std::thread([this]() { run_loop(); });
    Logger::log(Logger::Level::kInfo, "WhatsApp channel started");
  }

  void stop() override {
    if (!running_.exchange(false)) {
      return;
    }
    out_cv_.notify_all();
    if (worker_.joinable()) {
      worker_.join();
    }
    connected_.store(false);
    Logger::log(Logger::Level::kInfo, "WhatsApp channel stopped");
  }

  void send(const OutboundMessage& msg) override {
    const std::string to = trim(msg.chat_id);
    const std::string text = trim(msg.content);
    if (to.empty() || text.empty()) {
      return;
    }

    {
      std::lock_guard<std::mutex> lock(out_mu_);
      outbox_.push_back(PendingSend{to, text});
    }
    Logger::log(Logger::Level::kInfo,
                "WhatsApp outbound queued to " + to + ": " +
                    text.substr(0, (std::min<std::size_t>)(text.size(), 120)));
    out_cv_.notify_one();
  }

 private:
  struct PendingSend {
    std::string to;
    std::string text;
  };

  static void ensure_global_init() {
    static std::once_flag flag;
    std::call_once(flag, []() { curl_global_init(CURL_GLOBAL_DEFAULT); });
  }

  static size_t discard_write(char* ptr, size_t size, size_t nmemb, void* userdata) {
    (void)ptr;
    (void)userdata;
    return size * nmemb;
  }

  static std::string strip_jid_domain(const std::string& s) {
    const auto p = s.find('@');
    return p == std::string::npos ? s : s.substr(0, p);
  }

  bool is_allowed_sender(const std::string& sender, const std::string& pn) const {
    if (allow_from_.empty()) {
      return true;
    }

    const std::string sender_id = strip_jid_domain(sender);
    const std::string pn_id = strip_jid_domain(pn);
    return allow_from_.contains(sender) || allow_from_.contains(sender_id) || allow_from_.contains(pn) ||
           allow_from_.contains(pn_id);
  }

  CURL* connect_bridge() const {
    ensure_global_init();
    CURL* curl = curl_easy_init();
    if (!curl) {
      return nullptr;
    }

    curl_easy_setopt(curl, CURLOPT_URL, bridge_url_.c_str());
    curl_easy_setopt(curl, CURLOPT_CONNECT_ONLY, 2L);  // websocket mode
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "attoclaw/0.1");
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &discard_write);

    const CURLcode rc = curl_easy_perform(curl);
    if (rc != CURLE_OK) {
      if (rc == CURLE_UNSUPPORTED_PROTOCOL) {
        Logger::log(Logger::Level::kWarn,
                    "WhatsApp bridge connect failed: libcurl lacks WebSocket protocol support. "
                    "Install vcpkg curl[websockets] and rebuild.");
      } else {
        Logger::log(Logger::Level::kWarn,
                    "WhatsApp bridge connect failed: " + std::string(curl_easy_strerror(rc)));
      }
      curl_easy_cleanup(curl);
      return nullptr;
    }

    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    if (http_code != 101) {
      Logger::log(Logger::Level::kWarn,
                  "WhatsApp bridge did not switch protocols (HTTP " + std::to_string(http_code) +
                      "). Check bridgeUrl and ensure the bridge is running.");
      curl_easy_cleanup(curl);
      return nullptr;
    }
    return curl;
  }

  bool ws_send_text(CURL* curl, const std::string& text) const {
    std::size_t offset = 0;
    while (running_.load() && offset < text.size()) {
      size_t sent = 0;
      const CURLcode rc = curl_ws_send(curl, text.data() + offset, text.size() - offset, &sent,
                                       static_cast<curl_off_t>(text.size()), CURLWS_TEXT);
      if (rc == CURLE_AGAIN) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        continue;
      }
      if (rc != CURLE_OK) {
        Logger::log(Logger::Level::kWarn,
                    "WhatsApp bridge send failed: " + std::string(curl_easy_strerror(rc)));
        return false;
      }
      if (sent == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        continue;
      }
      offset += sent;
    }
    return offset == text.size();
  }

  bool ws_send_json(CURL* curl, const json& payload) const { return ws_send_text(curl, payload.dump()); }

  bool flush_outbox(CURL* curl) {
    std::deque<PendingSend> pending;
    {
      std::lock_guard<std::mutex> lock(out_mu_);
      if (outbox_.empty()) {
        return true;
      }
      pending.swap(outbox_);
    }

    for (const auto& msg : pending) {
      const json payload = {{"type", "send"}, {"to", msg.to}, {"text", msg.text}};
      if (!ws_send_json(curl, payload)) {
        return false;
      }
      Logger::log(Logger::Level::kInfo,
                  "WhatsApp outbound sent to bridge for " + msg.to);
    }
    return true;
  }

  void handle_bridge_json(const std::string& raw) {
    if (trim(raw).empty()) {
      return;
    }

    try {
      const json data = json::parse(raw);
      const std::string type = data.value("type", "");
      if (type == "message") {
        const std::string sender = data.value("sender", "");
        const std::string pn = data.value("pn", "");
        const std::string content = data.value("content", "");
        if (sender.empty() || trim(content).empty()) {
          return;
        }
        if (!is_allowed_sender(sender, pn)) {
          return;
        }

        const std::string user = !trim(pn).empty() ? pn : sender;
        const std::string sender_id = strip_jid_domain(user);
        Logger::log(Logger::Level::kInfo,
                    "WhatsApp inbound from " + sender_id + " (" + sender + "): " +
                        content.substr(0, (std::min<std::size_t>)(content.size(), 120)));
        handle_message(sender_id, sender, content);
        return;
      }

      if (type == "status") {
        const std::string status = data.value("status", "");
        if (status == "connected") {
          connected_.store(true);
        } else if (status == "disconnected") {
          connected_.store(false);
        }
        return;
      }

      if (type == "qr") {
        Logger::log(Logger::Level::kInfo, "WhatsApp QR received. Run `attoclaw channels login` to scan.");
        return;
      }

      if (type == "error") {
        Logger::log(Logger::Level::kWarn, "WhatsApp bridge error: " + data.value("error", "unknown"));
        return;
      }

      if (type == "sent") {
        return;
      }
    } catch (const std::exception& e) {
      Logger::log(Logger::Level::kWarn,
                  std::string("WhatsApp bridge payload parse error: ") + e.what());
    }
  }

  bool receive_once(CURL* curl, std::string& text_accumulator) {
    char buffer[8192];
    std::size_t nrecv = 0;
    const struct curl_ws_frame* meta = nullptr;
    const CURLcode rc = curl_ws_recv(curl, buffer, sizeof(buffer), &nrecv, &meta);

    if (rc == CURLE_AGAIN) {
      return true;
    }
    if (rc != CURLE_OK) {
      Logger::log(Logger::Level::kWarn,
                  "WhatsApp bridge recv failed: " + std::string(curl_easy_strerror(rc)));
      return false;
    }
    if (!meta) {
      return true;
    }
    if ((meta->flags & CURLWS_CLOSE) != 0) {
      Logger::log(Logger::Level::kInfo, "WhatsApp bridge closed connection.");
      return false;
    }
    if ((meta->flags & CURLWS_TEXT) == 0 && (meta->flags & CURLWS_CONT) == 0) {
      return true;
    }

    if (nrecv > 0) {
      text_accumulator.append(buffer, nrecv);
    }
    if (meta->bytesleft == 0) {
      handle_bridge_json(text_accumulator);
      text_accumulator.clear();
    }
    return true;
  }

  void run_loop() {
    while (running_.load()) {
      CURL* curl = connect_bridge();
      if (!curl) {
        for (int i = 0; i < 20 && running_.load(); ++i) {
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        continue;
      }

      if (!bridge_token_.empty()) {
        if (!ws_send_json(curl, json{{"type", "auth"}, {"token", bridge_token_}})) {
          curl_easy_cleanup(curl);
          for (int i = 0; i < 20 && running_.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
          }
          continue;
        }
      }

      connected_.store(true);
      Logger::log(Logger::Level::kInfo, "WhatsApp bridge connected");

      std::string text_accumulator;
      while (running_.load()) {
        if (!flush_outbox(curl)) {
          break;
        }
        if (!receive_once(curl, text_accumulator)) {
          break;
        }

        std::unique_lock<std::mutex> lock(out_mu_);
        out_cv_.wait_for(lock, std::chrono::milliseconds(100),
                         [this]() { return !running_.load() || !outbox_.empty(); });
      }

      connected_.store(false);
      curl_easy_cleanup(curl);

      if (!running_.load()) {
        break;
      }
      for (int i = 0; i < 20 && running_.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
    }
  }

  WhatsAppChannelConfig config_;
  std::string bridge_url_;
  std::string bridge_token_;
  std::unordered_set<std::string> allow_from_;

  std::atomic<bool> running_{false};
  std::atomic<bool> connected_{false};
  std::thread worker_;

  std::mutex out_mu_;
  std::condition_variable out_cv_;
  std::deque<PendingSend> outbox_;
};

}  // namespace attoclaw


