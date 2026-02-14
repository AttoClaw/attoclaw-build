#pragma once

#include <algorithm>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "attoclaw/common.hpp"

namespace attoclaw {

struct SessionMessage {
  std::string role;
  std::string content;
  std::string timestamp;
  std::vector<std::string> tools_used;
};

struct Session {
  std::string key;
  std::vector<SessionMessage> messages;
  std::string created_at{now_iso8601()};
  std::string updated_at{now_iso8601()};
  std::size_t last_consolidated{0};

  void add_message(const std::string& role, const std::string& content,
                   const std::vector<std::string>& tools_used = {}) {
    messages.push_back(SessionMessage{role, content, now_iso8601(), tools_used});
    updated_at = now_iso8601();
  }

  json get_history(std::size_t max_messages = 500) const {
    json out = json::array();
    const std::size_t start = messages.size() > max_messages ? messages.size() - max_messages : 0;
    for (std::size_t i = start; i < messages.size(); ++i) {
      out.push_back({{"role", messages[i].role}, {"content", messages[i].content}});
    }
    return out;
  }

  void clear() {
    messages.clear();
    last_consolidated = 0;
    updated_at = now_iso8601();
  }
};

class SessionManager {
 public:
  explicit SessionManager(const fs::path& workspace)
      : workspace_(workspace), sessions_dir_(expand_user_path("~/.attoclaw/sessions")) {
    std::error_code ec;
    fs::create_directories(sessions_dir_, ec);
  }

  Session& get_or_create(const std::string& key) {
    auto it = cache_.find(key);
    if (it != cache_.end()) {
      return it->second;
    }

    Session s = load(key);
    s.key = key;
    cache_[key] = std::move(s);
    return cache_[key];
  }

  void save(const Session& session) {
    const fs::path path = session_path(session.key);
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);

    std::ofstream out(path, std::ios::out | std::ios::binary | std::ios::trunc);
    if (!out) {
      Logger::log(Logger::Level::kError, "Cannot save session: " + session.key);
      return;
    }

    json meta = {{"_type", "metadata"},
                 {"created_at", session.created_at},
                 {"updated_at", session.updated_at},
                 {"last_consolidated", session.last_consolidated}};
    out << meta.dump() << "\n";

    for (const auto& m : session.messages) {
      json row = {{"role", m.role}, {"content", m.content}, {"timestamp", m.timestamp}};
      if (!m.tools_used.empty()) {
        row["tools_used"] = m.tools_used;
      }
      out << row.dump() << "\n";
    }

    cache_[session.key] = session;
  }

  void invalidate(const std::string& key) { cache_.erase(key); }

 private:
  Session load(const std::string& key) const {
    Session s;
    s.key = key;

    const fs::path path = session_path(key);
    if (!fs::exists(path)) {
      return s;
    }

    std::ifstream in(path);
    if (!in) {
      return s;
    }

    std::string line;
    bool first = true;
    while (std::getline(in, line)) {
      line = trim(line);
      if (line.empty()) {
        continue;
      }

      try {
        const json row = json::parse(line);
        if (first && row.value("_type", "") == "metadata") {
          s.created_at = row.value("created_at", s.created_at);
          s.updated_at = row.value("updated_at", s.updated_at);
          s.last_consolidated = row.value("last_consolidated", std::size_t{0});
          first = false;
          continue;
        }

        SessionMessage msg;
        msg.role = row.value("role", "assistant");
        msg.content = row.value("content", "");
        msg.timestamp = row.value("timestamp", now_iso8601());
        if (row.contains("tools_used") && row["tools_used"].is_array()) {
          msg.tools_used = row["tools_used"].get<std::vector<std::string>>();
        }
        s.messages.push_back(std::move(msg));
      } catch (...) {
      }
      first = false;
    }

    return s;
  }

  fs::path session_path(const std::string& key) const {
    std::string safe;
    safe.reserve(key.size());
    for (char c : key) {
      if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-') {
        safe.push_back(c);
      } else {
        safe.push_back('_');
      }
    }
    return sessions_dir_ / (safe + ".jsonl");
  }

  fs::path workspace_;
  fs::path sessions_dir_;
  mutable std::unordered_map<std::string, Session> cache_;
};

}  // namespace attoclaw

