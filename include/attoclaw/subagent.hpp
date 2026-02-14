#pragma once

#include <atomic>
#include <string>
#include <thread>

#include "attoclaw/common.hpp"
#include "attoclaw/events.hpp"
#include "attoclaw/message_bus.hpp"
#include "attoclaw/provider.hpp"
#include "attoclaw/tools.hpp"

namespace attoclaw {

class SubagentManager : public SpawnManager {
 public:
  SubagentManager(LLMProvider* provider, fs::path workspace, MessageBus* bus, std::string model,
                  double temperature, double top_p, int max_tokens, std::string brave_api_key,
                  int exec_timeout_seconds, bool restrict_to_workspace)
      : provider_(provider),
        workspace_(std::move(workspace)),
        bus_(bus),
        model_(std::move(model)),
        temperature_(temperature),
        top_p_(top_p),
        max_tokens_(max_tokens),
        brave_api_key_(std::move(brave_api_key)),
        exec_timeout_seconds_(exec_timeout_seconds),
        restrict_to_workspace_(restrict_to_workspace),
        running_count_(std::make_shared<std::atomic<int>>(0)) {}

  std::string spawn(const std::string& task, const std::string& label, const std::string& origin_channel,
                    const std::string& origin_chat_id) override {
    if (!provider_ || !bus_) {
      return "Error: Subagent runtime is unavailable";
    }

    const std::string task_id = random_id(8);
    const std::string display_label = trim(label).empty() ? summarize_label(task) : label;
    auto running_count = running_count_;
    running_count->fetch_add(1);

    // Detached subagent worker thread.
    std::thread([=]() {
      run_subagent(task_id, task, display_label, origin_channel, origin_chat_id);
      running_count->fetch_sub(1);
    }).detach();

    return "Subagent [" + display_label + "] started (id: " + task_id +
           "). I'll notify you when it completes.";
  }

  int running_count() const { return running_count_->load(); }

 private:
  static bool strip_vision_flag(std::string& text) {
    bool found = false;
    const std::string token = "--vision";
    std::size_t pos = 0;
    while ((pos = text.find(token, pos)) != std::string::npos) {
      const bool left_ok = pos == 0 || std::isspace(static_cast<unsigned char>(text[pos - 1])) != 0;
      const std::size_t end = pos + token.size();
      const bool right_ok = end >= text.size() || std::isspace(static_cast<unsigned char>(text[end])) != 0;
      if (left_ok && right_ok) {
        text.erase(pos, token.size());
        found = true;
      } else {
        pos = end;
      }
    }
    if (found) {
      text = trim(text);
    }
    return found;
  }

  static std::string summarize_label(const std::string& task) {
    constexpr std::size_t kMax = 30;
    if (task.size() <= kMax) {
      return task;
    }
    return task.substr(0, kMax) + "...";
  }

  std::string subagent_prompt() const {
    std::ostringstream out;
    out << "# Subagent\n\n";
    out << "Current time: " << now_iso8601() << "\n\n";
    out << "You are a background subagent. Complete only the requested task.\n";
    out << "Rules:\n";
    out << "1. Stay focused on the assigned task.\n";
    out << "2. Use tools when needed.\n";
    out << "3. Return a concise final result.\n";
    out << "4. Do not start side tasks.\n";
    out << "Workspace: " << workspace_.string() << "\n";
    return out.str();
  }

  void run_subagent(const std::string& task_id, const std::string& task, const std::string& label,
                    const std::string& origin_channel, const std::string& origin_chat_id) const {
    std::string final_result;
    std::string status = "ok";
    std::string task_text = task;
    const bool vision_enabled = strip_vision_flag(task_text);

    try {
      ToolRegistry tools;
      std::optional<fs::path> allowed_dir;
      if (restrict_to_workspace_) {
        allowed_dir = workspace_;
      }
      tools.register_tool(std::make_shared<ReadFileTool>(allowed_dir));
      tools.register_tool(std::make_shared<WriteFileTool>(allowed_dir));
      tools.register_tool(std::make_shared<EditFileTool>(allowed_dir));
      tools.register_tool(std::make_shared<ListDirTool>(allowed_dir));
      tools.register_tool(std::make_shared<ExecTool>(exec_timeout_seconds_, workspace_, restrict_to_workspace_));
      tools.register_tool(std::make_shared<WebSearchTool>(brave_api_key_, 5));
      tools.register_tool(std::make_shared<WebFetchTool>());
      tools.register_tool(std::make_shared<SystemInspectTool>());
      tools.register_tool(std::make_shared<AppControlTool>());
      tools.register_tool(std::make_shared<ScreenCaptureTool>(vision_enabled));

      json messages = json::array();
      messages.push_back({{"role", "system"}, {"content", subagent_prompt()}});
      messages.push_back({{"role", "user"}, {"content", task_text}});

      constexpr int kMaxIterations = 15;
      for (int i = 0; i < kMaxIterations; ++i) {
        const LLMResponse resp =
            provider_->chat(messages, tools.definitions(), model_, max_tokens_, temperature_, top_p_);

        if (resp.has_tool_calls()) {
          json tool_call_dicts = json::array();
          for (const auto& tc : resp.tool_calls) {
            tool_call_dicts.push_back(
                {{"id", tc.id},
                 {"type", "function"},
                 {"function", {{"name", tc.name}, {"arguments", tc.arguments.dump()}}}});
          }
          messages.push_back(
              {{"role", "assistant"}, {"content", resp.content}, {"tool_calls", tool_call_dicts}});

          for (const auto& tc : resp.tool_calls) {
            const std::string result = tools.execute(tc.name, tc.arguments);
            messages.push_back({{"role", "tool"},
                                {"tool_call_id", tc.id},
                                {"name", tc.name},
                                {"content", result}});
          }
        } else {
          final_result = resp.content;
          break;
        }
      }

      if (trim(final_result).empty()) {
        final_result = "Task completed but no final response was generated.";
      }
    } catch (const std::exception& e) {
      status = "error";
      final_result = std::string("Error: ") + e.what();
    }

    const std::string status_text = (status == "ok") ? "completed successfully" : "failed";
    const std::string announce_content =
        "[Subagent '" + label + "' " + status_text +
        "]\n\nTask: " + task + "\n\nResult:\n" + final_result +
        "\n\nSummarize this naturally for the user. Keep it brief (1-2 sentences). "
        "Do not mention technical details like subagent internals or task IDs.";

    InboundMessage announce;
    announce.channel = "system";
    announce.sender_id = "subagent";
    announce.chat_id = origin_channel + ":" + origin_chat_id;
    announce.content = announce_content;
    bus_->publish_inbound(announce);
    Logger::log(Logger::Level::kInfo, "Subagent [" + task_id + "] finished with status: " + status);
  }

  LLMProvider* provider_{nullptr};
  fs::path workspace_;
  MessageBus* bus_{nullptr};
  std::string model_;
  double temperature_{0.7};
  double top_p_{0.9};
  int max_tokens_{4096};
  std::string brave_api_key_;
  int exec_timeout_seconds_{60};
  bool restrict_to_workspace_{false};
  std::shared_ptr<std::atomic<int>> running_count_;
};

}  // namespace attoclaw

