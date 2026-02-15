#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "attoclaw/common.hpp"
#include "attoclaw/context.hpp"
#include "attoclaw/cron.hpp"
#include "attoclaw/events.hpp"
#include "attoclaw/external_cli.hpp"
#include "attoclaw/memory.hpp"
#include "attoclaw/metrics.hpp"
#include "attoclaw/message_bus.hpp"
#include "attoclaw/provider.hpp"
#include "attoclaw/session.hpp"
#include "attoclaw/subagent.hpp"
#include "attoclaw/tools.hpp"

namespace attoclaw {

class CronTool : public Tool {
 public:
  explicit CronTool(CronService* cron) : cron_(cron) {}

  void set_context(std::string channel, std::string chat_id) {
    channel_ = std::move(channel);
    chat_id_ = std::move(chat_id);
  }

  std::string name() const override { return "cron"; }
  std::string description() const override {
    return "Schedule reminders and recurring tasks (actions: add, list, remove)";
  }
  json parameters() const override {
    return json{{"type", "object"},
                {"properties",
                 {{"action", {{"type", "string"}, {"enum", json::array({"add", "list", "remove"})}}},
                  {"message", {{"type", "string"}}},
                  {"every_seconds", {{"type", "integer"}}},
                  {"cron_expr", {{"type", "string"}}},
                  {"at", {{"type", "string"}}},
                  {"job_id", {{"type", "string"}}}}},
                {"required", json::array({"action"})}};
  }

  std::string execute(const json& params) override {
    if (!cron_) {
      return "Error: cron service unavailable";
    }

    const std::string action = params.value("action", "");
    if (action == "list") {
      auto jobs = cron_->list_jobs(true);
      if (jobs.empty()) {
        return "No scheduled jobs.";
      }
      std::ostringstream out;
      out << "Scheduled jobs:\n";
      for (const auto& j : jobs) {
        out << "- " << j.name << " (id: " << j.id << ", " << j.schedule.kind << ")\n";
      }
      return trim(out.str());
    }

    if (action == "remove") {
      const std::string id = params.value("job_id", "");
      if (id.empty()) {
        return "Error: job_id is required for remove";
      }
      return cron_->remove_job(id) ? ("Removed job " + id) : ("Job " + id + " not found");
    }

    if (action == "add") {
      const std::string message = params.value("message", "");
      if (message.empty()) {
        return "Error: message is required for add";
      }

      CronSchedule schedule;
      bool delete_after = false;

      if (params.contains("every_seconds") && params["every_seconds"].is_number_integer()) {
        schedule.kind = "every";
        schedule.every_ms = params["every_seconds"].get<int64_t>() * 1000;
      } else if (params.contains("cron_expr") && params["cron_expr"].is_string() &&
                 !trim(params["cron_expr"].get<std::string>()).empty()) {
        schedule.kind = "cron";
        schedule.expr = trim(params["cron_expr"].get<std::string>());
      } else if (params.contains("at") && params["at"].is_string()) {
        const int64_t at_ms = parse_iso_to_ms(params["at"].get<std::string>());
        if (at_ms <= 0) {
          return "Error: invalid --at datetime (expected YYYY-MM-DDTHH:MM:SS)";
        }
        schedule.kind = "at";
        schedule.at_ms = at_ms;
        delete_after = true;
      } else {
        return "Error: either every_seconds, cron_expr, or at is required";
      }

      const auto job = cron_->add_job(message.substr(0, 30), schedule, message, true, channel_, chat_id_, delete_after);
      return "Created job '" + job.name + "' (id: " + job.id + ")";
    }

    return "Error: unknown cron action";
  }

 private:
  static int64_t parse_iso_to_ms(const std::string& text) {
    std::tm tm{};
    std::istringstream ss(text);
    ss >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");
    if (ss.fail()) {
      return 0;
    }
    const std::time_t t = std::mktime(&tm);
    if (t <= 0) {
      return 0;
    }
    return static_cast<int64_t>(t) * 1000;
  }

  CronService* cron_{nullptr};
  std::string channel_;
  std::string chat_id_;
};

class AgentLoop {
 public:
  AgentLoop(MessageBus* bus, LLMProvider* provider, fs::path workspace, std::string model, int max_iterations,
            double temperature, double top_p, int max_tokens, int memory_window, std::string brave_api_key,
            std::string transcribe_api_key, std::string transcribe_api_base, std::string transcribe_model,
            int transcribe_timeout_seconds, int exec_timeout_seconds, bool restrict_to_workspace,
            CronService* cron_service = nullptr)
      : bus_(bus),
        provider_(provider),
        workspace_(std::move(workspace)),
        model_(std::move(model)),
        max_iterations_(max_iterations),
        temperature_(temperature),
        top_p_(top_p),
        max_tokens_(max_tokens),
        memory_window_(memory_window),
        brave_api_key_(std::move(brave_api_key)),
        transcribe_api_key_(std::move(transcribe_api_key)),
        transcribe_api_base_(std::move(transcribe_api_base)),
        transcribe_model_(std::move(transcribe_model)),
        transcribe_timeout_seconds_(transcribe_timeout_seconds),
        exec_timeout_seconds_(exec_timeout_seconds),
        restrict_to_workspace_(restrict_to_workspace),
        context_(workspace_),
        sessions_(workspace_),
        subagents_(provider_, workspace_, bus_, model_, temperature_, top_p_, max_tokens_, brave_api_key_,
                   transcribe_api_key_, transcribe_api_base_, transcribe_model_, transcribe_timeout_seconds_,
                   exec_timeout_seconds_, restrict_to_workspace_),
        cron_(cron_service) {
    register_default_tools();
  }

  ~AgentLoop() { stop(); }

  void run() {
    if (running_.exchange(true)) {
      return;
    }
    worker_ = std::thread([this]() {
      Logger::log(Logger::Level::kInfo, "Agent loop started");
      while (running_.load()) {
        InboundMessage msg = bus_->consume_inbound();
        if (!running_.load()) {
          break;
        }

        try {
          auto response = process_message(msg, std::nullopt, {});
          if (response.has_value()) {
            bus_->publish_outbound(*response);
          }
        } catch (const std::exception& e) {
          OutboundMessage err;
          err.channel = msg.channel;
          err.chat_id = msg.chat_id;
          err.content = std::string("Sorry, I encountered an error: ") + e.what();
          bus_->publish_outbound(err);
        }
      }
    });
  }

  void stop() {
    if (!running_.exchange(false)) {
      return;
    }
    if (bus_) {
      bus_->publish_inbound(InboundMessage{"system", "stop", "stop", "stop"});
    }
    if (worker_.joinable()) {
      worker_.join();
    }
  }

  std::string process_direct(const std::string& content, const std::string& session_key = "cli:direct",
                             const std::string& channel = "cli", const std::string& chat_id = "direct") {
    const InboundMessage msg{channel, "user", chat_id, content};
    auto response = process_message(msg, session_key, {});
    std::string out = response.has_value() ? response->content : std::string();
    out += drain_system_announcements(channel, chat_id);
    return out;
  }

  std::string process_direct_stream(const std::string& content,
                                    const std::function<void(const std::string&)>& on_delta,
                                    const std::string& session_key = "cli:direct",
                                    const std::string& channel = "cli", const std::string& chat_id = "direct") {
    const InboundMessage msg{channel, "user", chat_id, content};
    auto response = process_message(msg, session_key, on_delta);
    std::string out = response.has_value() ? response->content : std::string();
    const std::string extra = drain_system_announcements(channel, chat_id);
    if (on_delta && !extra.empty()) {
      on_delta(extra);
    }
    out += extra;
    return out;
  }

 private:
  class RequestRunScope {
   public:
    RequestRunScope(AgentLoop* owner, bool vision_enabled) : owner_(owner) {
      owner_->task_in_progress_.store(true);
      owner_->cancel_requested_.store(false);
      if (owner_->screen_capture_tool_) {
        owner_->screen_capture_tool_->set_enabled(vision_enabled);
      }
    }

    ~RequestRunScope() {
      if (owner_->screen_capture_tool_) {
        owner_->screen_capture_tool_->set_enabled(false);
      }
      owner_->flush_deferred_inbound();
      owner_->cancel_requested_.store(false);
      owner_->task_in_progress_.store(false);
    }

   private:
    AgentLoop* owner_;
  };

  void register_default_tools() {
    std::optional<fs::path> allowed_dir;
    if (restrict_to_workspace_) {
      allowed_dir = workspace_;
    }

    tools_.register_tool(std::make_shared<ReadFileTool>(allowed_dir));
    tools_.register_tool(std::make_shared<WriteFileTool>(allowed_dir));
    tools_.register_tool(std::make_shared<EditFileTool>(allowed_dir));
    tools_.register_tool(std::make_shared<ListDirTool>(allowed_dir));

    tools_.register_tool(
        std::make_shared<ExecTool>(exec_timeout_seconds_, workspace_, restrict_to_workspace_));
    tools_.register_tool(std::make_shared<WebSearchTool>(brave_api_key_, 5));
    tools_.register_tool(std::make_shared<WebFetchTool>());
    if (!trim(transcribe_api_base_).empty()) {
      tools_.register_tool(std::make_shared<TranscribeTool>(transcribe_api_key_, transcribe_api_base_,
                                                            transcribe_model_, transcribe_timeout_seconds_));
    }
    tools_.register_tool(std::make_shared<SystemInspectTool>());
    tools_.register_tool(std::make_shared<AppControlTool>());
    screen_capture_tool_ = std::make_shared<ScreenCaptureTool>(false);
    tools_.register_tool(screen_capture_tool_);

    message_tool_ = std::make_shared<MessageTool>([this](const OutboundMessage& msg) {
      if (bus_) {
        bus_->publish_outbound(msg);
      }
    });
    tools_.register_tool(message_tool_);

    spawn_tool_ = std::make_shared<SpawnTool>(&subagents_);
    tools_.register_tool(spawn_tool_);

    if (cron_) {
      cron_tool_ = std::make_shared<CronTool>(cron_);
      tools_.register_tool(cron_tool_);
    }
  }

  void set_tool_context(const std::string& channel, const std::string& chat_id) {
    if (message_tool_) {
      message_tool_->set_context(channel, chat_id);
    }
    if (cron_tool_) {
      cron_tool_->set_context(channel, chat_id);
    }
    if (spawn_tool_) {
      spawn_tool_->set_context(channel, chat_id);
    }
  }

  std::pair<std::string, std::vector<std::string>> run_agent_loop(
      const json& initial_messages, const std::string& channel, const std::string& chat_id,
      const std::function<void(const std::string&)>& on_stream_delta) {
    json messages = initial_messages;
    std::vector<std::string> tools_used;
    std::string final_content;
    std::string last_assistant_content;

    for (int iteration = 0; iteration < max_iterations_; ++iteration) {
      if (poll_for_stop_signal(channel, chat_id)) {
        final_content = "Stopped.";
        break;
      }

      std::string stream_buffer;
      const LLMResponse resp = on_stream_delta
                                   ? provider_->chat_stream(
                                         messages, tools_.definitions(), model_, max_tokens_, temperature_, top_p_,
                                         [&](const std::string& piece) { stream_buffer += piece; })
                                   : provider_->chat(messages, tools_.definitions(), model_, max_tokens_,
                                                     temperature_, top_p_);
      if (on_stream_delta && !resp.has_tool_calls() && !stream_buffer.empty()) {
        on_stream_delta(stream_buffer);
      }
      if (!trim(resp.content).empty()) {
        last_assistant_content = resp.content;
      }

      if (poll_for_stop_signal(channel, chat_id)) {
        final_content = "Stopped.";
        break;
      }

      if (resp.has_tool_calls()) {
        json tool_call_dicts = json::array();
        for (const auto& tc : resp.tool_calls) {
          tool_call_dicts.push_back(
              {{"id", tc.id},
               {"type", "function"},
               {"function", {{"name", tc.name}, {"arguments", tc.arguments.dump()}}}});
        }

        context_.add_assistant_message(messages, resp.content, tool_call_dicts, resp.reasoning_content);

        for (const auto& tc : resp.tool_calls) {
          if (poll_for_stop_signal(channel, chat_id)) {
            final_content = "Stopped.";
            break;
          }
          tools_used.push_back(tc.name);
          const std::string result = tools_.execute(tc.name, tc.arguments);
          context_.add_tool_result(messages, tc.id, tc.name, result);
        }

        if (!final_content.empty()) {
          break;
        }

        messages.push_back({{"role", "user"}, {"content", "Reflect on the results and decide next steps."}});
      } else {
        final_content = resp.content;
        break;
      }
    }

    if (final_content.empty()) {
      final_content = last_assistant_content.empty() ? "I've completed processing but have no response to give."
                                                     : last_assistant_content;
    }

    return {final_content, tools_used};
  }

  std::optional<OutboundMessage> process_message(
      const InboundMessage& msg, std::optional<std::string> session_override,
      const std::function<void(const std::string&)>& on_stream_delta) {
    if (msg.channel == "system" && msg.content == "stop") {
      return std::nullopt;
    }

    if (msg.channel == "system") {
      return process_system_message(msg);
    }

    const std::string key = session_override.has_value() ? *session_override : msg.session_key();
    Session& session = sessions_.get_or_create(key);

    const std::string command = trim(msg.content);
    if (to_lower(command) == "/new") {
      session.clear();
      sessions_.save(session);
      sessions_.invalidate(session.key);
      return OutboundMessage{msg.channel, msg.chat_id, "New session started."};
    }
    if (to_lower(command) == "/help") {
      return OutboundMessage{msg.channel, msg.chat_id,
                             "AttoClaw commands:\n/new - Start a new conversation\n/stop - Stop current task\n/help - Show commands\n\n"
                             "Message suffixes:\n--codex - Route this prompt to Codex CLI\n--gemini - Route this prompt to Gemini CLI\n"
                             "--vision - Enable screen context (can be combined as: <prompt> --vision --codex)"};
    }
    if (to_lower(command) == "/stop") {
      if (!task_in_progress_.load()) {
        return OutboundMessage{msg.channel, msg.chat_id, "No active task is running."};
      }
      cancel_requested_.store(true);
      return OutboundMessage{msg.channel, msg.chat_id, "Stopping current task..."};
    }

    if (static_cast<int>(session.messages.size()) > memory_window_) {
      consolidate_memory(session, false);
    }

    const ParsedExternalRequest parsed = parse_external_request(msg.content);
    std::string user_content = parsed.prompt;

    if (!msg.media.empty() && !trim(transcribe_api_base_).empty()) {
      std::ostringstream media_block;
      media_block << "\n\n[Media attachments]\n";
      int idx = 1;
      for (const auto& p : msg.media) {
        if (trim(p).empty()) {
          continue;
        }
        media_block << "- audio[" << idx << "]: " << p << "\n";
        ++idx;
      }

      std::ostringstream transcript_block;
      transcript_block << "\n[Transcription]\n";
      TranscribeTool transcriber(transcribe_api_key_, transcribe_api_base_, transcribe_model_,
                                 transcribe_timeout_seconds_);
      idx = 1;
      for (const auto& p : msg.media) {
        if (trim(p).empty()) {
          continue;
        }
        fs::path audio_path = expand_user_path(p);
#ifndef _WIN32
        if (!audio_path.has_extension() || audio_path.extension() != ".wav") {
          if (!command_exists_in_path("ffmpeg")) {
            std::string note;
            try_install_linux_package("ffmpeg", 240, &note);
          }
          if (command_exists_in_path("ffmpeg")) {
            const fs::path out_dir = expand_user_path("~/.attoclaw") / "inbox" / "converted";
            std::error_code ec;
            fs::create_directories(out_dir, ec);
            const fs::path out = out_dir / (audio_path.stem().string() + "_" + std::to_string(now_ms()) + ".wav");
            const std::string in_q = sh_single_quote(fs::absolute(audio_path).string());
            const std::string out_q = sh_single_quote(fs::absolute(out).string());
            const std::string cmd =
                "sh -lc \"ffmpeg -y -hide_banner -loglevel error -i " + in_q + " -ac 1 -ar 16000 " + out_q + "\"";
            const CommandResult conv = run_command_capture(cmd, 240);
            if (conv.ok && fs::exists(out, ec)) {
              audio_path = out;
            }
          }
        }
#endif

        metrics().inc("transcribe.total");
        const std::string t = transcriber.execute(json{{"path", audio_path.string()}});
        if (t.rfind("Error:", 0) == 0) {
          metrics().inc("transcribe.error");
        } else {
          metrics().inc("transcribe.ok");
        }
        transcript_block << "- audio[" << idx << "]:\n" << t << "\n";
        ++idx;
      }

      if (user_content.empty()) {
        user_content = trim(msg.content);
      }
      user_content = trim(user_content + media_block.str() + transcript_block.str());
    }
    if (parsed.vision_enabled && is_headless_server()) {
      return OutboundMessage{msg.channel, msg.chat_id,
                             "Vision is unavailable on headless server (DISPLAY/WAYLAND_DISPLAY not set)."};
    }

    if (parsed.external_cli.has_value()) {
      const std::string final_content = run_external_cli(*parsed.external_cli, workspace_, parsed.vision_enabled);
      session.add_message("user", parsed.external_cli->prompt.empty() ? trim(msg.content) : parsed.external_cli->prompt);
      session.add_message("assistant", final_content, {parsed.external_cli->name});
      sessions_.save(session);

      OutboundMessage out;
      out.channel = msg.channel;
      out.chat_id = msg.chat_id;
      out.content = final_content;
      out.metadata = msg.metadata;
      return out;
    }

    const bool vision_enabled = parsed.vision_enabled;

    set_tool_context(msg.channel, msg.chat_id);
    RequestRunScope run_scope(this, vision_enabled);

    json history = session.get_history(memory_window_);
    json initial_messages = context_.build_messages(history, user_content, {}, msg.channel, msg.chat_id);

    auto [final_content, tools_used] = run_agent_loop(initial_messages, msg.channel, msg.chat_id, on_stream_delta);

    session.add_message("user", user_content);
    session.add_message("assistant", final_content, tools_used);
    sessions_.save(session);

    OutboundMessage out;
    out.channel = msg.channel;
    out.chat_id = msg.chat_id;
    out.content = final_content;
    out.metadata = msg.metadata;
    return out;
  }

  std::optional<OutboundMessage> process_system_message(const InboundMessage& msg) {
    std::string origin_channel = "cli";
    std::string origin_chat_id = "direct";

    const auto p = msg.chat_id.find(':');
    if (p != std::string::npos) {
      origin_channel = msg.chat_id.substr(0, p);
      origin_chat_id = msg.chat_id.substr(p + 1);
    } else {
      origin_chat_id = msg.chat_id;
    }

    const std::string key = origin_channel + ":" + origin_chat_id;
    Session& session = sessions_.get_or_create(key);

    set_tool_context(origin_channel, origin_chat_id);
    RequestRunScope run_scope(this, false);
    json initial = context_.build_messages(session.get_history(memory_window_), msg.content, {}, origin_channel,
                                           origin_chat_id);

    auto [final_content, _tools] = run_agent_loop(initial, origin_channel, origin_chat_id, {});

    session.add_message("user", "[System] " + msg.content);
    session.add_message("assistant", final_content);
    sessions_.save(session);

    return OutboundMessage{origin_channel, origin_chat_id, final_content};
  }

  std::string drain_system_announcements(const std::string& origin_channel, const std::string& origin_chat_id) {
    if (!bus_) {
      return "";
    }
    const std::string target = origin_channel + ":" + origin_chat_id;
    std::vector<InboundMessage> deferred;
    std::string appended;

    // Drain a bounded batch to avoid starving other producers.
    constexpr int kMax = 32;
    for (int i = 0; i < kMax; ++i) {
      auto pending = bus_->try_consume_inbound();
      if (!pending.has_value()) {
        break;
      }
      InboundMessage msg = std::move(*pending);
      if (msg.channel == "system" && msg.chat_id == target) {
        auto response = process_system_message(msg);
        if (response.has_value() && !trim(response->content).empty()) {
          if (!appended.empty()) {
            appended += "\n\n";
          } else {
            appended = "\n\n";
          }
          appended += response->content;
        }
      } else {
        deferred.push_back(std::move(msg));
      }
    }

    for (const auto& m : deferred) {
      bus_->publish_inbound(m);
    }
    return appended;
  }

  void consolidate_memory(Session& session, bool archive_all) {
    MemoryStore memory(workspace_);

    std::size_t keep_count = archive_all ? 0 : static_cast<std::size_t>((std::max)(1, memory_window_ / 2));
    if (session.messages.size() <= keep_count) {
      return;
    }

    std::size_t start = archive_all ? 0 : session.last_consolidated;
    std::size_t end = archive_all ? session.messages.size() : (session.messages.size() - keep_count);

    if (start >= end || end > session.messages.size()) {
      return;
    }

    std::ostringstream history;
    history << "[" << now_iso8601().substr(0, 16) << "] Session summary\n";
    for (std::size_t i = start; i < end; ++i) {
      const auto& m = session.messages[i];
      history << "[" << m.timestamp.substr(0, 16) << "] " << to_upper(m.role) << ": " << m.content << "\n";
    }

    memory.append_history(history.str());

    if (archive_all) {
      session.last_consolidated = 0;
      session.messages.clear();
    } else {
      session.last_consolidated = end;
    }
  }

  static std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
  }

  static std::string to_upper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return s;
  }

  bool poll_for_stop_signal(const std::string& active_channel, const std::string& active_chat_id) {
    if (cancel_requested_.load()) {
      return true;
    }
    if (!bus_) {
      return false;
    }

    constexpr int kBatch = 8;
    for (int i = 0; i < kBatch; ++i) {
      auto pending = bus_->try_consume_inbound();
      if (!pending.has_value()) {
        break;
      }

      InboundMessage msg = std::move(*pending);
      const std::string cmd = to_lower(trim(msg.content));
      const bool is_target_session =
          msg.channel == active_channel && msg.chat_id == active_chat_id;
      if (is_target_session && cmd == "/stop") {
        const bool first = !cancel_requested_.exchange(true);
        if (first) {
          bus_->publish_outbound(OutboundMessage{active_channel, active_chat_id, "Stopping current task..."});
        }
      } else {
        stash_deferred_inbound(std::move(msg));
      }
    }
    return cancel_requested_.load();
  }

  void stash_deferred_inbound(InboundMessage msg) {
    std::lock_guard<std::mutex> lock(deferred_mu_);
    deferred_inbound_.push_back(std::move(msg));
  }

  void flush_deferred_inbound() {
    if (!bus_) {
      return;
    }
    std::vector<InboundMessage> pending;
    {
      std::lock_guard<std::mutex> lock(deferred_mu_);
      pending.swap(deferred_inbound_);
    }
    for (const auto& msg : pending) {
      bus_->publish_inbound(msg);
    }
  }

  MessageBus* bus_;
  LLMProvider* provider_;
  fs::path workspace_;
  std::string model_;
  int max_iterations_;
  double temperature_;
  double top_p_;
  int max_tokens_;
  int memory_window_;
  std::string brave_api_key_;
  std::string transcribe_api_key_;
  std::string transcribe_api_base_;
  std::string transcribe_model_;
  int transcribe_timeout_seconds_{180};
  int exec_timeout_seconds_;
  bool restrict_to_workspace_;

  ContextBuilder context_;
  SessionManager sessions_;
  ToolRegistry tools_;
  SubagentManager subagents_;

  std::shared_ptr<MessageTool> message_tool_;
  std::shared_ptr<SpawnTool> spawn_tool_;
  std::shared_ptr<CronTool> cron_tool_;
  std::shared_ptr<ScreenCaptureTool> screen_capture_tool_;

  CronService* cron_{nullptr};
  std::atomic<bool> cancel_requested_{false};
  std::atomic<bool> task_in_progress_{false};
  std::mutex deferred_mu_;
  std::vector<InboundMessage> deferred_inbound_;

  std::atomic<bool> running_{false};
  std::thread worker_;
};

}  // namespace attoclaw
