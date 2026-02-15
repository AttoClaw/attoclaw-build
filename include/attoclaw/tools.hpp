#pragma once

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <regex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "attoclaw/common.hpp"
#include "attoclaw/events.hpp"
#include "attoclaw/http.hpp"
#include "attoclaw/vision.hpp"

namespace attoclaw {

class Tool {
 public:
  virtual ~Tool() = default;

  virtual std::string name() const = 0;
  virtual std::string description() const = 0;
  virtual json parameters() const = 0;
  virtual std::string execute(const json& params) = 0;

  virtual std::vector<std::string> validate(const json& params) const {
    json schema = parameters();
    std::vector<std::string> errors;
    validate_node(params, schema, "parameter", errors);
    return errors;
  }

  json to_schema() const {
    return json{{"type", "function"},
                {"function", {{"name", name()}, {"description", description()}, {"parameters", parameters()}}}};
  }

 private:
  static bool type_ok(const json& value, const std::string& type_name) {
    if (type_name == "string") {
      return value.is_string();
    }
    if (type_name == "integer") {
      return value.is_number_integer();
    }
    if (type_name == "number") {
      return value.is_number();
    }
    if (type_name == "boolean") {
      return value.is_boolean();
    }
    if (type_name == "array") {
      return value.is_array();
    }
    if (type_name == "object") {
      return value.is_object();
    }
    return true;
  }

  static void validate_node(const json& value, const json& schema, const std::string& label,
                            std::vector<std::string>& errors) {
    const std::string t = schema.value("type", "");
    if (!t.empty() && !type_ok(value, t)) {
      errors.push_back(label + " should be " + t);
      return;
    }

    if (schema.contains("enum") && schema["enum"].is_array()) {
      bool ok = false;
      for (const auto& e : schema["enum"]) {
        if (e == value) {
          ok = true;
          break;
        }
      }
      if (!ok) {
        errors.push_back(label + " has invalid enum value");
      }
    }

    if (t == "object") {
      const json props = schema.value("properties", json::object());
      if (schema.contains("required") && schema["required"].is_array()) {
        for (const auto& req : schema["required"]) {
          const std::string key = req.get<std::string>();
          if (!value.contains(key)) {
            errors.push_back("missing required " + label + "." + key);
          }
        }
      }

      for (auto it = value.begin(); it != value.end(); ++it) {
        if (props.contains(it.key())) {
          validate_node(it.value(), props[it.key()], label + "." + it.key(), errors);
        }
      }
    }

    if (t == "array" && schema.contains("items") && value.is_array()) {
      std::size_t i = 0;
      for (const auto& item : value) {
        validate_node(item, schema["items"], label + "[" + std::to_string(i) + "]", errors);
        ++i;
      }
    }
  }
};

class ToolRegistry {
 public:
  void register_tool(std::shared_ptr<Tool> tool) {
    tools_[tool->name()] = std::move(tool);
    rebuild_definitions_cache();
  }

  std::shared_ptr<Tool> get(const std::string& name) const {
    auto it = tools_.find(name);
    return it == tools_.end() ? nullptr : it->second;
  }

  const json& definitions() const { return definitions_cache_; }

  std::string execute(const std::string& name, const json& params) {
    auto it = tools_.find(name);
    if (it == tools_.end()) {
      return "Error: Tool '" + name + "' not found";
    }

    const auto errors = it->second->validate(params);
    if (!errors.empty()) {
      std::string msg = "Error: Invalid parameters for tool '" + name + "': ";
      for (std::size_t i = 0; i < errors.size(); ++i) {
        msg += errors[i];
        if (i + 1 < errors.size()) {
          msg += "; ";
        }
      }
      return msg;
    }

    try {
      return it->second->execute(params);
    } catch (const std::exception& e) {
      return std::string("Error executing ") + name + ": " + e.what();
    }
  }

 private:
  void rebuild_definitions_cache() {
    definitions_cache_ = json::array();
    for (const auto& [_, tool] : tools_) {
      definitions_cache_.push_back(tool->to_schema());
    }
  }

  std::unordered_map<std::string, std::shared_ptr<Tool>> tools_;
  json definitions_cache_ = json::array();
};

inline fs::path resolve_path(const std::string& path, const std::optional<fs::path>& allowed_dir) {
  fs::path resolved = fs::weakly_canonical(expand_user_path(path));
  if (!allowed_dir.has_value()) {
    return resolved;
  }

  fs::path allowed = fs::weakly_canonical(allowed_dir.value());
  auto mismatch_pair = std::mismatch(allowed.begin(), allowed.end(), resolved.begin(), resolved.end());
  const bool inside = mismatch_pair.first == allowed.end();
  if (!inside) {
    throw std::runtime_error("Path is outside allowed directory");
  }
  return resolved;
}

inline std::string shell_escape_single_quotes(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (char c : s) {
    if (c == '\'') {
      out += "''";
    } else {
      out.push_back(c);
    }
  }
  return out;
}

class ReadFileTool : public Tool {
 public:
  explicit ReadFileTool(std::optional<fs::path> allowed_dir) : allowed_dir_(std::move(allowed_dir)) {}

  std::string name() const override { return "read_file"; }
  std::string description() const override { return "Read file content from a path"; }
  json parameters() const override {
    return json{{"type", "object"},
                {"properties", {{"path", {{"type", "string"}, {"description", "Path to file"}}}}},
                {"required", json::array({"path"})}};
  }

  std::string execute(const json& params) override {
    const auto path = params.value("path", "");
    const fs::path p = resolve_path(path, allowed_dir_);
    if (!fs::exists(p)) {
      return "Error: File not found: " + path;
    }
    if (!fs::is_regular_file(p)) {
      return "Error: Not a file: " + path;
    }
    return read_text_file(p);
  }

 private:
  std::optional<fs::path> allowed_dir_;
};

class WriteFileTool : public Tool {
 public:
  explicit WriteFileTool(std::optional<fs::path> allowed_dir) : allowed_dir_(std::move(allowed_dir)) {}

  std::string name() const override { return "write_file"; }
  std::string description() const override { return "Write text content to file"; }
  json parameters() const override {
    return json{{"type", "object"},
                {"properties",
                 {{"path", {{"type", "string"}, {"description", "Path to file"}}},
                  {"content", {{"type", "string"}, {"description", "Content to write"}}}}},
                {"required", json::array({"path", "content"})}};
  }

  std::string execute(const json& params) override {
    const auto path = params.value("path", "");
    const auto content = params.value("content", "");
    const fs::path p = resolve_path(path, allowed_dir_);
    if (!write_text_file(p, content)) {
      return "Error: failed to write file";
    }
    return "Successfully wrote " + std::to_string(content.size()) + " bytes to " + path;
  }

 private:
  std::optional<fs::path> allowed_dir_;
};

class EditFileTool : public Tool {
 public:
  explicit EditFileTool(std::optional<fs::path> allowed_dir) : allowed_dir_(std::move(allowed_dir)) {}

  std::string name() const override { return "edit_file"; }
  std::string description() const override {
    return "Edit file by replacing old_text with new_text once";
  }
  json parameters() const override {
    return json{{"type", "object"},
                {"properties",
                 {{"path", {{"type", "string"}}},
                  {"old_text", {{"type", "string"}}},
                  {"new_text", {{"type", "string"}}}}},
                {"required", json::array({"path", "old_text", "new_text"})}};
  }

  std::string execute(const json& params) override {
    const auto path = params.value("path", "");
    const auto old_text = params.value("old_text", "");
    const auto new_text = params.value("new_text", "");

    const fs::path p = resolve_path(path, allowed_dir_);
    if (!fs::exists(p)) {
      return "Error: File not found: " + path;
    }

    std::string content = read_text_file(p);
    const std::size_t pos = content.find(old_text);
    if (pos == std::string::npos) {
      return "Error: old_text not found in file";
    }
    if (content.find(old_text, pos + old_text.size()) != std::string::npos) {
      return "Warning: old_text appears multiple times; provide a more specific pattern";
    }

    content.replace(pos, old_text.size(), new_text);
    if (!write_text_file(p, content)) {
      return "Error: failed to save edited file";
    }
    return "Successfully edited " + path;
  }

 private:
  std::optional<fs::path> allowed_dir_;
};

class ListDirTool : public Tool {
 public:
  explicit ListDirTool(std::optional<fs::path> allowed_dir) : allowed_dir_(std::move(allowed_dir)) {}

  std::string name() const override { return "list_dir"; }
  std::string description() const override { return "List files and folders in directory"; }
  json parameters() const override {
    return json{{"type", "object"},
                {"properties", {{"path", {{"type", "string"}, {"description", "Directory path"}}}}},
                {"required", json::array({"path"})}};
  }

  std::string execute(const json& params) override {
    const auto path = params.value("path", "");
    const fs::path p = resolve_path(path, allowed_dir_);
    if (!fs::exists(p)) {
      return "Error: Directory not found: " + path;
    }
    if (!fs::is_directory(p)) {
      return "Error: Not a directory: " + path;
    }

    std::vector<std::string> rows;
    for (const auto& entry : fs::directory_iterator(p)) {
      const std::string prefix = entry.is_directory() ? "[DIR] " : "[FILE] ";
      rows.push_back(prefix + entry.path().filename().string());
    }
    std::sort(rows.begin(), rows.end());

    if (rows.empty()) {
      return "Directory is empty";
    }

    std::ostringstream out;
    for (const auto& row : rows) {
      out << row << "\n";
    }
    return trim(out.str());
  }

 private:
  std::optional<fs::path> allowed_dir_;
};

class ExecTool : public Tool {
 public:
  ExecTool(int timeout_seconds, fs::path working_dir, bool restrict_to_workspace)
      : timeout_seconds_(timeout_seconds), working_dir_(std::move(working_dir)),
        restrict_to_workspace_(restrict_to_workspace) {}

  std::string name() const override { return "exec"; }
  std::string description() const override { return "Execute shell command and return output"; }
  json parameters() const override {
    return json{{"type", "object"},
                {"properties",
                 {{"command", {{"type", "string"}}}, {"working_dir", {{"type", "string"}}}}},
                {"required", json::array({"command"})}};
  }

  std::string execute(const json& params) override {
    const std::string command = params.value("command", "");
    const std::string requested_dir = params.value("working_dir", "");
    const fs::path cwd = requested_dir.empty() ? working_dir_ : fs::weakly_canonical(expand_user_path(requested_dir));

    const std::string guard = guard_command(command, cwd);
    if (!guard.empty()) {
      return guard;
    }

    std::string cmd;
#ifdef _WIN32
    cmd = "cd /d \"" + cwd.string() + "\" && " + command;
#else
    cmd = "cd \"" + cwd.string() + "\" && " + command;
#endif

    const CommandResult res = run_command_capture(cmd, timeout_seconds_);
    std::string output = trim(res.output);
    if (output.empty()) {
      output = "(no output)";
    }
    if (!res.ok) {
      output += "\nExit code: " + std::to_string(res.exit_code);
    }

    constexpr std::size_t kMaxLen = 10000;
    if (output.size() > kMaxLen) {
      output.resize(kMaxLen);
      output += "\n... (truncated)";
    }

    return output;
  }

 private:
  std::string guard_command(const std::string& command, const fs::path& cwd) const {
    static const std::vector<std::regex> deny_patterns = {
        std::regex(R"(\brm\s+-[rf]{1,2}\b)", std::regex::icase),
        std::regex(R"(\bdel\s+/[fq]\b)", std::regex::icase),
        std::regex(R"(\brmdir\s+/s\b)", std::regex::icase),
        std::regex(R"(\b(format|mkfs|diskpart|shutdown|reboot|poweroff)\b)", std::regex::icase),
    };

    for (const auto& re : deny_patterns) {
      if (std::regex_search(command, re)) {
        return "Error: Command blocked by safety guard";
      }
    }

    if (restrict_to_workspace_ && (command.find("../") != std::string::npos || command.find("..\\") != std::string::npos)) {
      return "Error: Command blocked by safety guard (path traversal detected)";
    }

    if (restrict_to_workspace_) {
      const fs::path base = fs::weakly_canonical(working_dir_);
      const fs::path cur = fs::weakly_canonical(cwd);
      auto mismatch_pair = std::mismatch(base.begin(), base.end(), cur.begin(), cur.end());
      if (mismatch_pair.first != base.end()) {
        return "Error: Command blocked (working dir outside workspace)";
      }
    }

    return "";
  }

  int timeout_seconds_;
  fs::path working_dir_;
  bool restrict_to_workspace_;
};

class SystemInspectTool : public Tool {
 public:
  std::string name() const override { return "system_inspect"; }
  std::string description() const override {
    return "Inspect local system state (processes, windows, disks, network, uptime).";
  }
  json parameters() const override {
    return json{{"type", "object"},
                {"properties",
                 {{"action",
                   {{"type", "string"},
                    {"enum", json::array({"processes", "windows", "disks", "network", "uptime"})}}},
                  {"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", 200}}}}},
                {"required", json::array({"action"})}};
  }

  std::string execute(const json& params) override {
    const std::string action = params.value("action", "");
    const int limit = std::clamp(params.value("limit", 20), 1, 200);

    std::string command;
#ifdef _WIN32
    if (action == "processes") {
      command =
          "powershell -NoProfile -ExecutionPolicy Bypass -Command "
          "\"Get-Process | Sort-Object CPU -Descending | "
          "Select-Object -First " +
          std::to_string(limit) +
          " ProcessName,Id,CPU,WS,MainWindowTitle | "
          "ConvertTo-Json -Depth 3\"";
    } else if (action == "windows") {
      command =
          "powershell -NoProfile -ExecutionPolicy Bypass -Command "
          "\"Get-Process | Where-Object { $_.MainWindowTitle -and $_.MainWindowTitle.Trim().Length -gt 0 } | "
          "Select-Object -First " +
          std::to_string(limit) +
          " ProcessName,Id,MainWindowTitle | "
          "ConvertTo-Json -Depth 3\"";
    } else if (action == "disks") {
      command =
          "powershell -NoProfile -ExecutionPolicy Bypass -Command "
          "\"Get-PSDrive -PSProvider FileSystem | "
          "Select-Object Name,Used,Free | ConvertTo-Json -Depth 3\"";
    } else if (action == "network") {
      command =
          "powershell -NoProfile -ExecutionPolicy Bypass -Command "
          "\"Get-NetTCPConnection -ErrorAction SilentlyContinue | "
          "Select-Object -First " +
          std::to_string(limit) +
          " LocalAddress,LocalPort,RemoteAddress,RemotePort,State,OwningProcess | "
          "ConvertTo-Json -Depth 3\"";
    } else if (action == "uptime") {
      command =
          "powershell -NoProfile -ExecutionPolicy Bypass -Command "
          "\"$os=Get-CimInstance Win32_OperatingSystem; "
          "$boot=$os.LastBootUpTime; "
          "$uptime=(Get-Date)-$boot; "
          "[pscustomobject]@{LastBoot=$boot; Uptime=$uptime.ToString()} | ConvertTo-Json -Depth 3\"";
    }
#else
    if (action == "processes") {
      command = "sh -lc \"ps -eo pid,ppid,pcpu,pmem,comm --sort=-pcpu | head -n " +
                std::to_string(limit + 1) + "\"";
    } else if (action == "windows") {
      command = "sh -lc \"command -v wmctrl >/dev/null 2>&1 && wmctrl -lp || echo 'wmctrl not available'\"";
    } else if (action == "disks") {
      command = "sh -lc \"df -h\"";
    } else if (action == "network") {
      command = "sh -lc \"ss -tan | head -n " + std::to_string(limit + 1) + "\"";
    } else if (action == "uptime") {
      command = "sh -lc \"uptime\"";
    }
#endif

    if (command.empty()) {
      return "Error: invalid action";
    }

    CommandResult res = run_command_capture(command, 30);
    std::string output = trim(res.output);
    if (output.empty()) {
      output = "(no output)";
    }
    if (!res.ok) {
      output += "\nExit code: " + std::to_string(res.exit_code);
    }
    if (output.size() > 12000) {
      output.resize(12000);
      output += "\n... (truncated)";
    }
    return output;
  }
};

class AppControlTool : public Tool {
 public:
  std::string name() const override { return "app_control"; }
  std::string description() const override {
    return "Control local apps: launch programs, terminate processes, or open URLs.";
  }
  json parameters() const override {
    return json{{"type", "object"},
                {"properties",
                 {{"action", {{"type", "string"}, {"enum", json::array({"launch", "terminate", "open_url"})}}},
                  {"target", {{"type", "string"}}},
                  {"args", {{"type", "string"}}}}},
                {"required", json::array({"action", "target"})}};
  }

  std::string execute(const json& params) override {
    const std::string action = params.value("action", "");
    const std::string target = trim(params.value("target", ""));
    const std::string args = params.value("args", "");
    if (target.empty()) {
      return "Error: target is required";
    }

    std::string command;
#ifdef _WIN32
    const std::string target_q = shell_escape_single_quotes(target);
    const std::string args_q = shell_escape_single_quotes(args);
    if (action == "launch") {
      command =
          "powershell -NoProfile -ExecutionPolicy Bypass -Command "
          "\"Start-Process -FilePath '" +
          target_q + "' -ArgumentList '" + args_q + "'\"";
    } else if (action == "open_url") {
      command =
          "powershell -NoProfile -ExecutionPolicy Bypass -Command "
          "\"Start-Process '" +
          target_q + "'\"";
    } else if (action == "terminate") {
      if (is_protected_process(target)) {
        return "Error: refusing to terminate protected system process";
      }
      const bool numeric =
          std::all_of(target.begin(), target.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
      if (numeric) {
        command =
            "powershell -NoProfile -ExecutionPolicy Bypass -Command "
            "\"Stop-Process -Id " +
            target + " -Force\"";
      } else {
        command =
            "powershell -NoProfile -ExecutionPolicy Bypass -Command "
            "\"Stop-Process -Name '" +
            target_q + "' -Force\"";
      }
    }
#else
    if (action == "launch") {
      command = "sh -lc \"" + target + " " + args + " >/dev/null 2>&1 &\"";
    } else if (action == "open_url") {
      command = "sh -lc \"xdg-open '" + target + "' >/dev/null 2>&1\"";
    } else if (action == "terminate") {
      command = "sh -lc \"pkill -f '" + target + "'\"";
    }
#endif

    if (command.empty()) {
      return "Error: invalid action";
    }

    CommandResult res = run_command_capture(command, 30);
    if (!res.ok) {
      std::string out = trim(res.output);
      if (out.empty()) {
        out = "command failed";
      }
      return "Error: " + out;
    }
    return "OK: " + action + " executed for target '" + target + "'";
  }

 private:
  static bool is_protected_process(std::string name) {
    std::transform(name.begin(), name.end(), name.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    static const std::vector<std::string> deny = {"csrss",   "wininit", "smss",
                                                  "services", "lsass",   "system"};
    return std::find(deny.begin(), deny.end(), name) != deny.end();
  }
};

class ScreenCaptureTool : public Tool {
 public:
  explicit ScreenCaptureTool(bool enabled = false) : enabled_(enabled) {}

  void set_enabled(bool enabled) { enabled_.store(enabled); }

  std::string name() const override { return "screen_capture"; }
  std::string description() const override {
    return "Capture the current screen and save as PNG. Returns the saved file path.";
  }
  json parameters() const override {
    return json{{"type", "object"},
                {"properties", {{"path", {{"type", "string"}}}}},
                {"required", json::array()}};
  }

  std::string execute(const json& params) override {
    if (!enabled_.load()) {
      return "Error: vision tools are disabled for this request. Add --vision in your message.";
    }
    if (is_headless_server()) {
      return "Error: vision is unavailable on headless server (DISPLAY/WAYLAND_DISPLAY not set).";
    }

    fs::path out;
    const std::string user_path = trim(params.value("path", ""));
    if (user_path.empty()) {
      const fs::path dir = expand_user_path("~/.attoclaw") / "screenshots";
      std::error_code ec;
      fs::create_directories(dir, ec);
      out = dir / ("screen_" + std::to_string(now_ms()) + ".png");
    } else {
      out = expand_user_path(user_path);
      std::error_code ec;
      fs::create_directories(out.parent_path(), ec);
    }

#ifdef _WIN32
    const std::string out_q = shell_escape_single_quotes(fs::absolute(out).string());
    const std::string command =
        "powershell -NoProfile -ExecutionPolicy Bypass -Command "
        "\"Add-Type -AssemblyName System.Windows.Forms; "
        "Add-Type -AssemblyName System.Drawing; "
        "$b=[System.Windows.Forms.SystemInformation]::VirtualScreen; "
        "$bmp=New-Object System.Drawing.Bitmap $b.Width,$b.Height; "
        "$g=[System.Drawing.Graphics]::FromImage($bmp); "
        "$g.CopyFromScreen($b.Left,$b.Top,0,0,$bmp.Size); "
        "$bmp.Save('" +
        out_q +
        "', [System.Drawing.Imaging.ImageFormat]::Png); "
        "$g.Dispose(); $bmp.Dispose(); "
        "Write-Output '" +
        out_q + "'\"";
    CommandResult res = run_command_capture(command, 30);
    if (!res.ok) {
      std::string out_err = trim(res.output);
      if (out_err.empty()) {
        out_err = "screenshot command failed";
      }
      return "Error: " + out_err;
    }
#else
    std::string dep_note;
    if (!ensure_vision_capture_dependencies(&dep_note)) {
      return "Error: " + dep_note;
    }

    const std::string path_abs = fs::absolute(out).string();
    const std::string path_q = sh_single_quote(path_abs);
    std::string command;
    if (command_exists_in_path("grim")) {
      command = "sh -lc \"grim " + path_q + "\"";
    } else if (command_exists_in_path("scrot")) {
      command = "sh -lc \"scrot " + path_q + "\"";
    } else {
      return "Error: no screenshot tool available (grim/scrot).";
    }
    CommandResult res = run_command_capture(command, 30);
    if (!res.ok) {
      std::string out_err = trim(res.output);
      if (out_err.empty()) {
        out_err = "screenshot command failed";
      }
      return "Error: " + out_err;
    }
#endif

    if (!fs::exists(out)) {
      return "Error: screenshot file was not created";
    }

    const auto bytes = static_cast<long long>(fs::file_size(out));
    return json{{"path", fs::absolute(out).string()}, {"bytes", bytes}, {"format", "png"}}.dump();
  }

 private:
  std::atomic<bool> enabled_{false};
};

class WebSearchTool : public Tool {
 public:
  WebSearchTool(std::string api_key, int max_results)
      : api_key_(std::move(api_key)), max_results_(std::clamp(max_results, 1, 10)) {}

  std::string name() const override { return "web_search"; }
  std::string description() const override { return "Search the web using Brave Search API"; }
  json parameters() const override {
    return json{{"type", "object"},
                {"properties",
                 {{"query", {{"type", "string"}}},
                  {"count", {{"type", "integer"}, {"minimum", 1}, {"maximum", 10}}}}},
                {"required", json::array({"query"})}};
  }

  std::string execute(const json& params) override {
    if (api_key_.empty()) {
      return "Error: BRAVE_API_KEY not configured";
    }

    const std::string query = params.value("query", "");
    const int count = std::clamp(params.value("count", max_results_), 1, 10);

    const std::string encoded = url_encode(query);
    const std::string url =
        "https://api.search.brave.com/res/v1/web/search?q=" + encoded + "&count=" + std::to_string(count);

    thread_local HttpClient client;
    HttpResponse resp = client.get(url,
                                   {
                                       {"Accept", "application/json"},
                                       {"X-Subscription-Token", api_key_},
                                   },
                                   15, true, 3);

    if (!resp.error.empty()) {
      return "Error: " + resp.error;
    }
    if (resp.status < 200 || resp.status >= 300) {
      return "Error: HTTP " + std::to_string(resp.status) + " - " + resp.body;
    }

    try {
      const json data = json::parse(resp.body);
      json results = data.value("web", json::object()).value("results", json::array());
      if (!results.is_array() || results.empty()) {
        return "No results for: " + query;
      }

      std::ostringstream out;
      out << "Results for: " << query << "\n\n";
      std::size_t idx = 1;
      for (const auto& item : results) {
        if (idx > static_cast<std::size_t>(count)) {
          break;
        }
        out << idx << ". " << item.value("title", "") << "\n";
        out << "   " << item.value("url", "") << "\n";
        const std::string desc = item.value("description", "");
        if (!desc.empty()) {
          out << "   " << desc << "\n";
        }
        ++idx;
      }
      return trim(out.str());

    } catch (const std::exception& e) {
      return std::string("Error parsing search response: ") + e.what();
    }
  }

 private:
  static std::string url_encode(const std::string& input) {
    static const char hex[] = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : input) {
      if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
        out.push_back(static_cast<char>(c));
      } else {
        out.push_back('%');
        out.push_back(hex[(c >> 4) & 0xF]);
        out.push_back(hex[c & 0xF]);
      }
    }
    return out;
  }

  std::string api_key_;
  int max_results_;
};

class TranscribeTool : public Tool {
 public:
  TranscribeTool(std::string api_key, std::string api_base, std::string model, int timeout_s)
      : api_key_(std::move(api_key)),
        api_base_(std::move(api_base)),
        model_(trim(model).empty() ? std::string() : std::move(model)),
        timeout_s_(std::clamp(timeout_s, 10, 900)) {}

  std::string name() const override { return "transcribe"; }
  std::string description() const override {
    return "Transcribe an audio file to text via an OpenAI-compatible /audio/transcriptions endpoint";
  }
  json parameters() const override {
    return json{{"type", "object"},
                {"properties",
                 {{"path", {{"type", "string"}}},
                  {"language", {{"type", "string"}}},
                  {"prompt", {{"type", "string"}}}}},
                {"required", json::array({"path"})}};
  }

  std::string execute(const json& params) override {
    if (trim(api_base_).empty()) {
      return "Error: transcription apiBase not configured";
    }

    if (trim(api_key_).empty() && !is_local_nim_endpoint(api_base_)) {
      return "Error: transcription apiKey not configured";
    }

    const std::string raw_path = trim(params.value("path", ""));
    if (raw_path.empty()) {
      return "Error: path is required";
    }
    const fs::path p = fs::weakly_canonical(expand_user_path(raw_path));
    std::error_code ec;
    if (!fs::exists(p, ec) || !fs::is_regular_file(p, ec)) {
      return "Error: file not found: " + p.string();
    }

    std::string base = trim(api_base_);
    while (!base.empty() && base.back() == '/') {
      base.pop_back();
    }
    const std::string url = base + "/audio/transcriptions";

    std::vector<MultipartField> fields;
    const std::string model = trim(model_);
    if (!model.empty() && model != "auto") {
      fields.push_back({"model", model});
    }
    const std::string language = trim(params.value("language", ""));
    if (!language.empty()) {
      fields.push_back({"language", language});
    }
    const std::string prompt = trim(params.value("prompt", ""));
    if (!prompt.empty()) {
      fields.push_back({"prompt", prompt});
    }

    thread_local HttpClient client;
    std::map<std::string, std::string> headers;
    if (!trim(api_key_).empty()) {
      headers["Authorization"] = "Bearer " + api_key_;
    }
    HttpResponse resp = client.post_multipart_file(
        url, headers,
        fields, "file", p, "", timeout_s_, true, 5);

    if (!resp.error.empty()) {
      return "Error: " + resp.error;
    }
    if (resp.status < 200 || resp.status >= 300) {
      return "Error: HTTP " + std::to_string(resp.status) + " - " + resp.body;
    }

    try {
      const json data = json::parse(resp.body);
      if (data.contains("text") && data["text"].is_string()) {
        return data["text"].get<std::string>();
      }
      if (data.contains("transcript") && data["transcript"].is_string()) {
        return data["transcript"].get<std::string>();
      }
      return resp.body;
    } catch (...) {
      return resp.body;
    }
  }

 private:
  std::string api_key_;
  std::string api_base_;
  std::string model_;
  int timeout_s_{180};

  static bool is_local_nim_endpoint(const std::string& base) {
    const std::string b = trim(base);
    // Riva ASR NIM default is http://localhost:9000/v1
    return b.find("://localhost") != std::string::npos || b.find("://127.0.0.1") != std::string::npos ||
           b.rfind("http://0.0.0.0", 0) == 0 || b.rfind("http://[::1]", 0) == 0;
  }
};

class WebFetchTool : public Tool {
 public:
  explicit WebFetchTool(int max_chars = 50000) : max_chars_(max_chars) {}

  std::string name() const override { return "web_fetch"; }
  std::string description() const override { return "Fetch URL and extract readable text"; }
  json parameters() const override {
    return json{{"type", "object"},
                {"properties",
                 {{"url", {{"type", "string"}}},
                  {"extractMode", {{"type", "string"}, {"enum", json::array({"markdown", "text"})}}},
                  {"maxChars", {{"type", "integer"}, {"minimum", 100}}}}},
                {"required", json::array({"url"})}};
  }

  std::string execute(const json& params) override {
    const std::string url = params.value("url", "");
    const std::string mode = params.value("extractMode", "markdown");
    const int max_chars = (std::max)(100, params.value("maxChars", max_chars_));

    if (!(starts_with(url, "http://") || starts_with(url, "https://"))) {
      return json({{"error", "Only http/https URLs allowed"}, {"url", url}}).dump();
    }

    thread_local HttpClient client;
    HttpResponse resp = client.get(url, { {"Accept", "*/*"} }, 30, true, 5);
    if (!resp.error.empty()) {
      return json({{"error", resp.error}, {"url", url}}).dump();
    }
    if (resp.status < 200 || resp.status >= 300) {
      return json({{"error", "HTTP " + std::to_string(resp.status)}, {"url", url}}).dump();
    }

    std::string text = resp.body;
    std::string extractor = "raw";
    if (looks_like_html(resp.body)) {
      extractor = mode == "markdown" ? "html_markdown" : "html_text";
      text = html_to_text(resp.body);
    }

    bool truncated = false;
    if (static_cast<int>(text.size()) > max_chars) {
      text.resize(max_chars);
      truncated = true;
    }

    return json({{"url", url},
                 {"finalUrl", resp.final_url},
                 {"status", resp.status},
                 {"extractor", extractor},
                 {"truncated", truncated},
                 {"length", text.size()},
                 {"text", text}})
        .dump();
  }

 private:
  static bool starts_with(const std::string& s, const std::string& pfx) {
    return s.size() >= pfx.size() && s.compare(0, pfx.size(), pfx) == 0;
  }

  static bool looks_like_html(const std::string& body) {
    const std::string head = body.substr(0, std::min<std::size_t>(512, body.size()));
    const std::string lower = to_lower(head);
    return lower.find("<html") != std::string::npos || lower.find("<!doctype") != std::string::npos;
  }

  static std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
  }

  static std::string html_to_text(std::string html) {
    html = std::regex_replace(html, std::regex(R"(<script[\s\S]*?</script>)", std::regex::icase), "");
    html = std::regex_replace(html, std::regex(R"(<style[\s\S]*?</style>)", std::regex::icase), "");
    html = std::regex_replace(html, std::regex(R"(<br\s*/?>)", std::regex::icase), "\n");
    html = std::regex_replace(html, std::regex(R"(</(p|div|section|article|h1|h2|h3|h4|h5|h6)>)", std::regex::icase), "\n\n");
    html = std::regex_replace(html, std::regex(R"(<[^>]+>)"), "");
    html = std::regex_replace(html, std::regex(R"([ \t]+)"), " ");
    html = std::regex_replace(html, std::regex(R"(\n{3,})"), "\n\n");
    return trim(html);
  }

  int max_chars_;
};

class MessageTool : public Tool {
 public:
  using SendCallback = std::function<void(const OutboundMessage&)>;

  explicit MessageTool(SendCallback cb) : callback_(std::move(cb)) {}

  void set_context(std::string channel, std::string chat_id) {
    default_channel_ = std::move(channel);
    default_chat_id_ = std::move(chat_id);
  }

  std::string name() const override { return "message"; }
  std::string description() const override { return "Send message to channel/chat"; }
  json parameters() const override {
    return json{{"type", "object"},
                {"properties",
                 {{"content", {{"type", "string"}}},
                  {"channel", {{"type", "string"}}},
                  {"chat_id", {{"type", "string"}}}}},
                {"required", json::array({"content"})}};
  }

  std::string execute(const json& params) override {
    const std::string content = params.value("content", "");
    const std::string channel = params.value("channel", default_channel_);
    const std::string chat_id = params.value("chat_id", default_chat_id_);

    if (channel.empty() || chat_id.empty()) {
      return "Error: No target channel/chat specified";
    }
    if (!callback_) {
      return "Error: Message callback not configured";
    }

    callback_(OutboundMessage{channel, chat_id, content});
    return "Message sent to " + channel + ":" + chat_id;
  }

 private:
  SendCallback callback_;
  std::string default_channel_;
  std::string default_chat_id_;
};

class SpawnManager {
 public:
  virtual ~SpawnManager() = default;
  virtual std::string spawn(const std::string& task, const std::string& label,
                            const std::string& origin_channel,
                            const std::string& origin_chat_id) = 0;
};

class SpawnTool : public Tool {
 public:
  explicit SpawnTool(SpawnManager* manager) : manager_(manager) {}

  void set_context(std::string channel, std::string chat_id) {
    origin_channel_ = std::move(channel);
    origin_chat_id_ = std::move(chat_id);
  }

  std::string name() const override { return "spawn"; }
  std::string description() const override {
    return "Spawn a background subagent to handle long-running tasks.";
  }
  json parameters() const override {
    return json{{"type", "object"},
                {"properties", {{"task", {{"type", "string"}}}, {"label", {{"type", "string"}}}}},
                {"required", json::array({"task"})}};
  }
  std::string execute(const json& params) override {
    const std::string task = params.value("task", "");
    const std::string label = params.value("label", "");
    if (!manager_) {
      return "Error: Spawn manager is not configured";
    }
    if (trim(task).empty()) {
      return "Error: task is required";
    }
    return manager_->spawn(task, label, origin_channel_, origin_chat_id_);
  }

 private:
  SpawnManager* manager_{nullptr};
  std::string origin_channel_{"cli"};
  std::string origin_chat_id_{"direct"};
};

}  // namespace attoclaw
