#pragma once

#include <mutex>
#include <string>
#include <unordered_map>

#include "attoclaw/common.hpp"

namespace attoclaw {

class Metrics {
 public:
  void inc(const std::string& key, uint64_t delta = 1) {
    std::lock_guard<std::mutex> lock(mu_);
    counters_[key] += delta;
  }

  json to_json() const {
    std::lock_guard<std::mutex> lock(mu_);
    json j = json::object();
    for (const auto& kv : counters_) {
      j[kv.first] = kv.second;
    }
    j["updatedAt"] = now_iso8601();
    return j;
  }

 private:
  mutable std::mutex mu_;
  std::unordered_map<std::string, uint64_t> counters_;
};

inline Metrics& metrics() {
  static Metrics m;
  return m;
}

inline fs::path default_metrics_path() {
  return expand_user_path("~/.attoclaw") / "state" / "metrics.json";
}

inline void write_metrics_snapshot(const fs::path& path = default_metrics_path()) {
  write_text_file(path, metrics().to_json().dump(2));
}

}  // namespace attoclaw

