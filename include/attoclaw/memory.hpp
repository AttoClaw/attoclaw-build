#pragma once

#include "attoclaw/common.hpp"

namespace attoclaw {

class MemoryStore {
 public:
  explicit MemoryStore(const fs::path& workspace) : memory_dir_(workspace / "memory") {
    std::error_code ec;
    fs::create_directories(memory_dir_, ec);
    memory_file_ = memory_dir_ / "MEMORY.md";
    history_file_ = memory_dir_ / "HISTORY.md";
  }

  std::string read_long_term() const { return read_text_file(memory_file_); }

  bool write_long_term(const std::string& content) const { return write_text_file(memory_file_, content); }

  void append_history(const std::string& entry) const {
    std::error_code ec;
    fs::create_directories(history_file_.parent_path(), ec);
    std::ofstream out(history_file_, std::ios::out | std::ios::app | std::ios::binary);
    if (!out) {
      return;
    }
    out << entry;
    if (!entry.empty() && entry.back() != '\n') {
      out << "\n";
    }
    out << "\n";
  }

  std::string memory_context() const {
    const std::string data = read_long_term();
    if (trim(data).empty()) {
      return "";
    }
    return "## Long-term Memory\n" + data;
  }

  fs::path memory_file() const { return memory_file_; }
  fs::path history_file() const { return history_file_; }

 private:
  fs::path memory_dir_;
  fs::path memory_file_;
  fs::path history_file_;
};

}  // namespace attoclaw

