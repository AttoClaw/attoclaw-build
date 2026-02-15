#pragma once

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

namespace attoclaw {

using json = nlohmann::json;
namespace fs = std::filesystem;

inline std::string trim(const std::string& s) {
  const auto start = s.find_first_not_of(" \t\r\n");
  if (start == std::string::npos) {
    return "";
  }
  const auto end = s.find_last_not_of(" \t\r\n");
  return s.substr(start, end - start + 1);
}

inline std::vector<std::string> chunk_text(const std::string& s, std::size_t max_chars) {
  std::vector<std::string> out;
  if (max_chars == 0) {
    return out;
  }
  if (s.size() <= max_chars) {
    out.push_back(s);
    return out;
  }
  std::size_t i = 0;
  while (i < s.size()) {
    const std::size_t n = (std::min)(max_chars, s.size() - i);
    out.push_back(s.substr(i, n));
    i += n;
  }
  return out;
}

inline std::string home_dir() {
#ifdef _WIN32
  const char* p = std::getenv("USERPROFILE");
  if (p && *p) {
    return std::string(p);
  }
  const char* drive = std::getenv("HOMEDRIVE");
  const char* path = std::getenv("HOMEPATH");
  if (drive && *drive && path && *path) {
    return std::string(drive) + std::string(path);
  }
#else
  const char* p = std::getenv("HOME");
#endif
  return p ? std::string(p) : std::string(".");
}

inline fs::path expand_user_path(const std::string& p) {
  if (!p.empty() && p[0] == '~') {
    std::string suffix = p.substr(1);
    while (!suffix.empty() && (suffix.front() == '/' || suffix.front() == '\\')) {
      suffix.erase(suffix.begin());
    }
    return fs::path(home_dir()) / suffix;
  }
  return fs::path(p);
}

inline std::string read_text_file(const fs::path& p) {
  std::ifstream in(p, std::ios::in | std::ios::binary);
  if (!in) {
    return "";
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

inline bool write_text_file(const fs::path& p, const std::string& content) {
  std::error_code ec;
  fs::create_directories(p.parent_path(), ec);
  std::ofstream out(p, std::ios::out | std::ios::binary | std::ios::trunc);
  if (!out) {
    return false;
  }
  out << content;
  return true;
}

inline std::string now_iso8601() {
  const auto now = std::chrono::system_clock::now();
  const auto t = std::chrono::system_clock::to_time_t(now);
  std::tm tm{};
#ifdef _WIN32
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  std::ostringstream ss;
  ss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S");
  return ss.str();
}

inline int64_t now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch())
      .count();
}

inline std::string random_id(std::size_t n = 8) {
  static constexpr char alphabet[] = "0123456789abcdefghijklmnopqrstuvwxyz";
  thread_local std::mt19937_64 rng{std::random_device{}()};
  std::uniform_int_distribution<std::size_t> d(0, sizeof(alphabet) - 2);

  std::string out;
  out.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    out.push_back(alphabet[d(rng)]);
  }
  return out;
}

struct CommandResult {
  bool ok{false};
  int exit_code{-1};
  std::string output;
};

inline CommandResult run_command_capture(const std::string& command, int timeout_s = 60) {
  const fs::path tmp = fs::temp_directory_path() / ("attoclaw_cmd_" + random_id(12) + ".log");
  const std::string wrapped = command + " > \"" + tmp.string() + "\" 2>&1";

  auto future = std::async(std::launch::async, [wrapped]() { return std::system(wrapped.c_str()); });
  if (future.wait_for(std::chrono::seconds(timeout_s)) == std::future_status::timeout) {
    return {false, -1, "Error: command timed out (process may continue in background)"};
  }

  const int code = future.get();
  std::string out = read_text_file(tmp);
  std::error_code ec;
  fs::remove(tmp, ec);
  return {code == 0, code, out};
}

class Logger {
 public:
  enum class Level { kInfo, kWarn, kError, kDebug };

  static void set_json(bool enabled) { json_mode().store(enabled); }
  static void set_min_level(Level level) { min_level() = level; }

  static void log(Level level, const std::string& msg) {
    if (level_rank(level) < level_rank(min_level())) {
      return;
    }
    static std::mutex mu;
    std::lock_guard<std::mutex> lock(mu);
    if (json_mode().load()) {
      json j;
      j["time"] = now_iso8601();
      j["level"] = level_name(level);
      j["msg"] = msg;
      std::cerr << j.dump() << "\n";
    } else {
      std::cerr << "[" << level_name(level) << "] " << msg << "\n";
    }
  }

 private:
  static int level_rank(Level level) {
    switch (level) {
      case Level::kDebug:
        return 0;
      case Level::kInfo:
        return 1;
      case Level::kWarn:
        return 2;
      case Level::kError:
      default:
        return 3;
    }
  }

  static std::atomic<bool>& json_mode() {
    static std::atomic<bool> v{false};
    return v;
  }

  static Level& min_level() {
    static Level v = Level::kInfo;
    return v;
  }

  static const char* level_name(Level level) {
    switch (level) {
      case Level::kInfo:
        return "INFO";
      case Level::kWarn:
        return "WARN";
      case Level::kError:
        return "ERROR";
      case Level::kDebug:
      default:
        return "DEBUG";
    }
  }
};

}  // namespace attoclaw
