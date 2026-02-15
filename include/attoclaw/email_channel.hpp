#pragma once

#include <atomic>
#include <chrono>
#include <cstring>
#include <ctime>
#include <string>
#include <thread>
#include <vector>

#include <curl/curl.h>

#include "attoclaw/channels.hpp"
#include "attoclaw/common.hpp"
#include "attoclaw/config.hpp"

namespace attoclaw {

class EmailChannel : public BaseChannel {
 public:
  EmailChannel(const EmailChannelConfig& config, MessageBus* bus) : BaseChannel("email", bus), config_(config) {
    ensure_global_init();
  }

  void start() override {
    // Outbound-only adapter; nothing to poll.
    running_.store(true);
    Logger::log(Logger::Level::kInfo, "Email channel started (outbound only)");
  }

  void stop() override {
    running_.store(false);
    Logger::log(Logger::Level::kInfo, "Email channel stopped");
  }

  void send(const OutboundMessage& msg) override {
    if (!running_.load()) {
      return;
    }
    if (trim(config_.smtp_url).empty()) {
      Logger::log(Logger::Level::kWarn, "Email send skipped: smtpUrl is empty");
      return;
    }
    if (trim(config_.from).empty()) {
      Logger::log(Logger::Level::kWarn, "Email send skipped: from is empty");
      return;
    }

    std::vector<std::string> recipients;
    if (!trim(msg.chat_id).empty()) {
      recipients.push_back(trim(msg.chat_id));
    } else {
      recipients = config_.default_to;
    }
    if (recipients.empty()) {
      Logger::log(Logger::Level::kWarn, "Email send skipped: no recipients (chat_id empty and defaultTo empty)");
      return;
    }

    const std::string subject = config_.subject_prefix.empty() ? "AttoClaw" : config_.subject_prefix;
    const std::string payload = build_email_payload(config_.from, recipients, subject, msg.content);

    CURL* curl = curl_easy_init();
    if (!curl) {
      Logger::log(Logger::Level::kWarn, "Email send failed: curl init failed");
      return;
    }

    struct curl_slist* rcpt_list = nullptr;
    for (const auto& r : recipients) {
      rcpt_list = curl_slist_append(rcpt_list, r.c_str());
    }

    UploadStatus upload;
    upload.data = payload.c_str();
    upload.len = payload.size();

    curl_easy_setopt(curl, CURLOPT_URL, config_.smtp_url.c_str());
    curl_easy_setopt(curl, CURLOPT_USERNAME, config_.username.c_str());
    curl_easy_setopt(curl, CURLOPT_PASSWORD, config_.password.c_str());
    curl_easy_setopt(curl, CURLOPT_MAIL_FROM, config_.from.c_str());
    curl_easy_setopt(curl, CURLOPT_MAIL_RCPT, rcpt_list);
    curl_easy_setopt(curl, CURLOPT_READFUNCTION, &read_cb);
    curl_easy_setopt(curl, CURLOPT_READDATA, &upload);
    curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "attoclaw/0.1");

    if (config_.use_ssl) {
      curl_easy_setopt(curl, CURLOPT_USE_SSL, static_cast<long>(CURLUSESSL_ALL));
    }

    const CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
      Logger::log(Logger::Level::kWarn, std::string("Email send failed: ") + curl_easy_strerror(res));
    }

    if (rcpt_list) {
      curl_slist_free_all(rcpt_list);
    }
    curl_easy_cleanup(curl);
  }

 private:
  struct UploadStatus {
    const char* data{nullptr};
    size_t len{0};
    size_t pos{0};
  };

  static size_t read_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    const size_t max = size * nmemb;
    auto* up = static_cast<UploadStatus*>(userdata);
    if (!up || !up->data || up->pos >= up->len) {
      return 0;
    }
    const size_t n = (std::min)(max, up->len - up->pos);
    std::memcpy(ptr, up->data + up->pos, n);
    up->pos += n;
    return n;
  }

  static void ensure_global_init() {
    static std::once_flag flag;
    std::call_once(flag, []() { curl_global_init(CURL_GLOBAL_DEFAULT); });
  }

  static std::string rfc2822_date() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[128];
    std::strftime(buf, sizeof(buf), "%a, %d %b %Y %H:%M:%S +0000", &tm);
    return std::string(buf);
  }

  static std::string join_recipients(const std::vector<std::string>& to) {
    std::ostringstream ss;
    for (std::size_t i = 0; i < to.size(); ++i) {
      ss << to[i];
      if (i + 1 < to.size()) {
        ss << ", ";
      }
    }
    return ss.str();
  }

  static std::string build_email_payload(const std::string& from, const std::vector<std::string>& to,
                                         const std::string& subject, const std::string& body) {
    std::ostringstream ss;
    ss << "Date: " << rfc2822_date() << "\r\n";
    ss << "To: " << join_recipients(to) << "\r\n";
    ss << "From: " << from << "\r\n";
    ss << "Subject: " << subject << "\r\n";
    ss << "MIME-Version: 1.0\r\n";
    ss << "Content-Type: text/plain; charset=utf-8\r\n";
    ss << "Content-Transfer-Encoding: 8bit\r\n";
    ss << "\r\n";
    ss << body << "\r\n";
    return ss.str();
  }

  EmailChannelConfig config_;
  std::atomic<bool> running_{false};
};

}  // namespace attoclaw
