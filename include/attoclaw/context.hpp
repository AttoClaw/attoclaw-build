#pragma once

#include <string>

#include "attoclaw/common.hpp"
#include "attoclaw/memory.hpp"
#include "attoclaw/skills.hpp"

namespace attoclaw {

class ContextBuilder {
 public:
  explicit ContextBuilder(fs::path workspace)
      : workspace_(std::move(workspace)), memory_(workspace_), skills_(workspace_) {}

  std::string build_system_prompt(const std::vector<std::string>& skill_names = {}) {
    std::vector<std::string> parts;
    parts.push_back(identity());

    const std::string bootstrap = load_bootstrap_files();
    if (!bootstrap.empty()) {
      parts.push_back(bootstrap);
    }

    const std::string mem = memory_.memory_context();
    if (!mem.empty()) {
      parts.push_back("# Memory\n\n" + mem);
    }

    if (!skill_names.empty()) {
      std::ostringstream ss;
      ss << "# Active Skills\n\n";
      for (const auto& name : skill_names) {
        const std::string content = skills_.load_skill(name);
        if (content.empty()) {
          continue;
        }
        ss << "## Skill: " << name << "\n\n" << content << "\n\n";
      }
      parts.push_back(trim(ss.str()));
    }

    const std::string summary = skills_.build_skills_summary();
    if (!summary.empty()) {
      parts.push_back("# Skills\n\nRead the skill file when needed using read_file.\n\n" + summary);
    }

    std::ostringstream out;
    for (std::size_t i = 0; i < parts.size(); ++i) {
      if (i) {
        out << "\n\n---\n\n";
      }
      out << parts[i];
    }
    return out.str();
  }

  json build_messages(const json& history, const std::string& current_message,
                      const std::vector<std::string>& skill_names = {},
                      const std::string& channel = "", const std::string& chat_id = "") {
    json messages = json::array();
    std::string system = build_system_prompt(skill_names);
    if (!channel.empty() && !chat_id.empty()) {
      system += "\n\n## Current Session\nChannel: " + channel + "\nChat ID: " + chat_id;
    }

    messages.push_back({{"role", "system"}, {"content", system}});

    for (const auto& msg : history) {
      messages.push_back(msg);
    }

    messages.push_back({{"role", "user"}, {"content", current_message}});
    return messages;
  }

  void add_assistant_message(json& messages, const std::string& content, const json& tool_calls = json::array(),
                             const std::string& reasoning_content = "") {
    json msg = {{"role", "assistant"}, {"content", content}};
    if (tool_calls.is_array() && !tool_calls.empty()) {
      msg["tool_calls"] = tool_calls;
    }
    if (!reasoning_content.empty()) {
      msg["reasoning_content"] = reasoning_content;
    }
    messages.push_back(std::move(msg));
  }

  void add_tool_result(json& messages, const std::string& tool_call_id, const std::string& name,
                       const std::string& result) {
    messages.push_back(
        {{"role", "tool"}, {"tool_call_id", tool_call_id}, {"name", name}, {"content", result}});
  }

 private:
  std::string identity() const {
    std::ostringstream ss;
    ss << "# AttoClaw\n\n";
    ss << "You are AttoClaw, a high-performance C++ personal AI assistant.\n";
    ss << "You can read/write/edit files, execute shell, fetch web content, inspect/control system apps, "
          "capture screenshots, and send messages.\n\n";
    ss << "## Current Time\n" << now_iso8601() << "\n\n";
    ss << "## Workspace\n" << workspace_.string() << "\n";
    ss << "- Long-term memory: " << (workspace_ / "memory" / "MEMORY.md").string() << "\n";
    ss << "- History log: " << (workspace_ / "memory" / "HISTORY.md").string() << "\n";
    ss << "- Skills: " << (workspace_ / "skills").string() << "\n\n";
    ss << "Respond directly to users. Use the message tool only for channel routing.";
    return ss.str();
  }

  std::string load_bootstrap_files() const {
    static const std::vector<std::string> files = {"AGENTS.md", "SOUL.md", "USER.md", "TOOLS.md", "IDENTITY.md"};

    std::ostringstream out;
    bool first = true;
    for (const auto& f : files) {
      const fs::path path = workspace_ / f;
      if (!fs::exists(path)) {
        continue;
      }
      if (!first) {
        out << "\n\n";
      }
      out << "## " << f << "\n\n" << read_text_file(path);
      first = false;
    }
    return out.str();
  }

  fs::path workspace_;
  MemoryStore memory_;
  SkillsLoader skills_;
};

}  // namespace attoclaw

