#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "attoclaw/common.hpp"

namespace attoclaw {

inline constexpr int kDefaultHeartbeatIntervalSec = 30 * 60;
inline constexpr const char* kHeartbeatPrompt =
    "Read HEARTBEAT.md in your workspace (if it exists).\n"
    "Follow any instructions or tasks listed there.\n"
    "If nothing needs attention, reply with just: HEARTBEAT_OK";

class HeartbeatService {
 public:
  using Callback = std::function<std::string(const std::string&)>;

  HeartbeatService(fs::path workspace, Callback cb, int interval_s = kDefaultHeartbeatIntervalSec,
                   bool enabled = true)
      : workspace_(std::move(workspace)), cb_(std::move(cb)), interval_s_(interval_s), enabled_(enabled) {}

  ~HeartbeatService() { stop(); }

  void start() {
    if (!enabled_ || running_.exchange(true)) {
      return;
    }
    worker_ = std::thread([this]() { loop(); });
  }

  void stop() {
    if (!running_.exchange(false)) {
      return;
    }
    cv_.notify_all();
    if (worker_.joinable()) {
      worker_.join();
    }
  }

  std::string trigger_now() {
    if (!cb_) {
      return "";
    }
    return cb_(kHeartbeatPrompt);
  }

 private:
  bool heartbeat_empty(const std::string& content) const {
    if (trim(content).empty()) {
      return true;
    }

    std::istringstream in(content);
    std::string line;
    while (std::getline(in, line)) {
      line = trim(line);
      if (line.empty() || starts_with(line, "#") || starts_with(line, "<!--") || line == "- [ ]" ||
          line == "* [ ]" || line == "- [x]" || line == "* [x]") {
        continue;
      }
      return false;
    }
    return true;
  }

  static bool starts_with(const std::string& s, const std::string& pfx) {
    return s.size() >= pfx.size() && s.compare(0, pfx.size(), pfx) == 0;
  }

  void loop() {
    while (running_.load()) {
      std::unique_lock<std::mutex> lock(wait_mu_);
      const bool stopped = cv_.wait_for(
          lock, std::chrono::seconds(interval_s_), [this]() { return !running_.load(); });
      lock.unlock();

      if (stopped || !running_.load()) {
        break;
      }

      const fs::path heartbeat = workspace_ / "HEARTBEAT.md";
      const std::string content = read_text_file(heartbeat);
      if (heartbeat_empty(content)) {
        continue;
      }

      try {
        if (cb_) {
          const std::string response = cb_(kHeartbeatPrompt);
          (void)response;
        }
      } catch (const std::exception& e) {
        Logger::log(Logger::Level::kError, std::string("Heartbeat callback failed: ") + e.what());
      }
    }
  }

  fs::path workspace_;
  Callback cb_;
  int interval_s_;
  bool enabled_;
  std::atomic<bool> running_{false};
  std::thread worker_;
  std::mutex wait_mu_;
  std::condition_variable cv_;
};

}  // namespace attoclaw

