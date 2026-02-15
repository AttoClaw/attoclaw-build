#pragma once

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "attoclaw/common.hpp"
#include "attoclaw/vision.hpp"

namespace attoclaw {

struct ExternalCliRoute {
  std::string name;
  std::string suffix;
  std::string prompt;
};

struct ParsedExternalRequest {
  std::string prompt;
  bool vision_enabled{false};
  std::optional<ExternalCliRoute> external_cli;
};

struct ExternalVisionContext {
  bool requested{false};
  bool captured{false};
  fs::path image_path;
  std::string ocr_text;
  std::string note;
};

struct ExternalCliCommandCandidate {
  std::string command;
  bool expect_json{false};
};

inline std::string cli_to_lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

inline bool has_suffix_token_ci(const std::string& text, const std::string& token_lower) {
  if (text.size() < token_lower.size()) {
    return false;
  }
  const std::size_t start = text.size() - token_lower.size();
  const std::string suffix_lower = cli_to_lower(text.substr(start));
  if (suffix_lower != token_lower) {
    return false;
  }
  return start == 0 || std::isspace(static_cast<unsigned char>(text[start - 1])) != 0;
}

inline bool strip_token_whole_word_ci(std::string& text, const std::string& token_lower) {
  bool found = false;
  std::size_t pos = 0;
  while (pos < text.size()) {
    const std::string lower = cli_to_lower(text);
    const std::size_t at = lower.find(token_lower, pos);
    if (at == std::string::npos) {
      break;
    }
    const bool left_ok = at == 0 || std::isspace(static_cast<unsigned char>(text[at - 1])) != 0;
    const std::size_t end = at + token_lower.size();
    const bool right_ok = end >= text.size() || std::isspace(static_cast<unsigned char>(text[end])) != 0;
    if (!left_ok || !right_ok) {
      pos = end;
      continue;
    }
    text.erase(at, token_lower.size());
    found = true;
    pos = at;
  }
  if (found) {
    text = trim(text);
  }
  return found;
}

inline ParsedExternalRequest parse_external_request(const std::string& content) {
  ParsedExternalRequest out;
  out.prompt = trim(content);
  if (out.prompt.empty()) {
    return out;
  }

  if (has_suffix_token_ci(out.prompt, "--codex")) {
    out.external_cli = ExternalCliRoute{"codex", "--codex", trim(out.prompt.substr(0, out.prompt.size() - 7))};
    out.prompt = out.external_cli->prompt;
  } else if (has_suffix_token_ci(out.prompt, "--gemini")) {
    out.external_cli = ExternalCliRoute{"gemini", "--gemini", trim(out.prompt.substr(0, out.prompt.size() - 8))};
    out.prompt = out.external_cli->prompt;
  }

  out.vision_enabled = strip_token_whole_word_ci(out.prompt, "--vision");
  if (out.external_cli.has_value()) {
    out.external_cli->prompt = out.prompt;
  }
  return out;
}

inline bool ensure_external_cli_available(const std::string& command, const std::string& npm_package,
                                          std::string* note = nullptr) {
  if (command_exists_in_path(command)) {
    return true;
  }
  if (!command_exists_in_path("npm")) {
    std::string install_note;
    (void)try_install_linux_package("nodejs", 300, &install_note);
    if (!command_exists_in_path("npm")) {
      (void)try_install_linux_package("npm", 300, &install_note);
    }
    if (!command_exists_in_path("npm")) {
      if (note) {
        *note = "npm is not installed and auto-install failed";
      }
      return false;
    }
  }

  static std::unordered_map<std::string, bool> install_attempted;
  if (!install_attempted[command]) {
    install_attempted[command] = true;
    CommandResult install = run_command_capture("npm install -g " + npm_package, 600);
    if (!install.ok) {
      const std::string lower = cli_to_lower(install.output);
      if (lower.find("could not find any python installation") != std::string::npos ||
          lower.find("find python") != std::string::npos) {
        std::string dep_note;
        (void)try_install_linux_package("python", 300, &dep_note);
        if (!command_exists_in_path("python3") && !command_exists_in_path("python")) {
          (void)try_install_linux_package("python3", 300, &dep_note);
        }
        install = run_command_capture("npm install -g " + npm_package, 600);
      }
    }
    if (!install.ok && note) {
      const std::string err = trim(install.output);
      if (!err.empty()) {
        *note = err;
      }
    }
  }

  if (command_exists_in_path(command)) {
    return true;
  }
  if (note && note->empty()) {
    *note = "auto-install failed for " + command;
  }
  return false;
}

inline std::string shell_quote_arg(const std::string& value) {
#ifdef _WIN32
  std::string out = "\"";
  for (char c : value) {
    if (c == '"') {
      out += "\\\"";
    } else {
      out.push_back(c);
    }
  }
  out.push_back('"');
  return out;
#else
  std::string out = "'";
  for (char c : value) {
    if (c == '\'') {
      out += "'\"'\"'";
    } else {
      out.push_back(c);
    }
  }
  out.push_back('\'');
  return out;
#endif
}

inline std::string shell_in_dir_command(const fs::path& dir, const std::string& command) {
#ifdef _WIN32
  return "cd /d \"" + dir.string() + "\" && " + command;
#else
  return "cd \"" + dir.string() + "\" && " + command;
#endif
}

inline bool looks_like_cli_usage_error(const std::string& output) {
  const std::string lower = cli_to_lower(output);
  return lower.find("usage:") != std::string::npos || lower.find("unknown command") != std::string::npos ||
         lower.find("unknown option") != std::string::npos || lower.find("invalid option") != std::string::npos ||
         lower.find("did you mean") != std::string::npos || lower.find("for more information, try '--help'") != std::string::npos;
}

inline std::string strip_ansi_sequences(const std::string& input) {
  std::string out;
  out.reserve(input.size());
  bool in_escape = false;
  bool in_csi = false;
  for (unsigned char c : input) {
    if (!in_escape) {
      if (c == 0x1B) {
        in_escape = true;
        in_csi = false;
      } else {
        out.push_back(static_cast<char>(c));
      }
      continue;
    }

    if (!in_csi) {
      in_csi = (c == '[');
      if (!in_csi) {
        in_escape = false;
      }
      continue;
    }

    if (c >= 0x40 && c <= 0x7E) {
      in_escape = false;
      in_csi = false;
    }
  }
  return out;
}

inline std::vector<std::string> split_lines_keep_nonempty_trimmed(const std::string& text) {
  std::vector<std::string> lines;
  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line)) {
    const std::string t = trim(line);
    if (!t.empty()) {
      lines.push_back(t);
    }
  }
  return lines;
}

inline std::string join_lines(const std::vector<std::string>& lines) {
  std::ostringstream out;
  for (std::size_t i = 0; i < lines.size(); ++i) {
    out << lines[i];
    if (i + 1 < lines.size()) {
      out << "\n";
    }
  }
  return out.str();
}

inline std::string extract_codex_json_message(const std::string& output) {
  std::vector<std::string> messages;
  std::istringstream in(output);
  std::string line;
  while (std::getline(in, line)) {
    const std::string t = trim(line);
    if (t.empty() || t.front() != '{') {
      continue;
    }
    try {
      const json row = json::parse(t);
      if (row.value("type", "") != "item.completed") {
        continue;
      }
      if (!row.contains("item") || !row["item"].is_object()) {
        continue;
      }
      const json item = row["item"];
      const std::string item_type = item.value("type", "");
      if (item_type != "agent_message" && item_type != "output_text") {
        continue;
      }
      const std::string text = trim(item.value("text", ""));
      if (text.empty()) {
        continue;
      }
      if (messages.empty() || messages.back() != text) {
        messages.push_back(text);
      }
    } catch (...) {
    }
  }
  return trim(join_lines(messages));
}

inline std::string extract_plain_cli_message(const std::string& cli_name, const std::string& output) {
  const std::string clean = strip_ansi_sequences(output);
  const std::vector<std::string> lines = split_lines_keep_nonempty_trimmed(clean);
  if (lines.empty()) {
    return "";
  }

  const std::string lower_cli = cli_to_lower(cli_name);
  if (lower_cli == "codex") {
    int marker = -1;
    for (std::size_t i = 0; i < lines.size(); ++i) {
      if (cli_to_lower(lines[i]) == "codex") {
        marker = static_cast<int>(i);
      }
    }

    if (marker >= 0) {
      std::vector<std::string> out;
      for (std::size_t i = static_cast<std::size_t>(marker + 1); i < lines.size(); ++i) {
        const std::string lower = cli_to_lower(lines[i]);
        if (lower == "tokens used" || lower.rfind("tokens used", 0) == 0) {
          break;
        }
        if (lower == "user" || lower == "assistant" || lower == "codex") {
          continue;
        }
        out.push_back(lines[i]);
      }
      const std::string extracted = trim(join_lines(out));
      if (!extracted.empty()) {
        return extracted;
      }
    }
  }

  return trim(clean);
}

inline ExternalVisionContext collect_external_vision_context(bool requested) {
  ExternalVisionContext ctx;
  ctx.requested = requested;
  if (!requested) {
    return ctx;
  }

#ifdef _WIN32
  auto frame = capture_vision_frame(1280, 70);
  if (!frame.has_value()) {
    ctx.note = "screen capture failed";
    return ctx;
  }
  ctx.captured = true;
  ctx.image_path = fs::absolute(frame->path);
#else
  if (!ensure_vision_capture_dependencies(&ctx.note)) {
    return ctx;
  }

  const fs::path dir = expand_user_path("~/.attoclaw") / "screenshots";
  std::error_code ec;
  fs::create_directories(dir, ec);
  const fs::path out = dir / ("external_vision_" + std::to_string(now_ms()) + ".png");
  const std::string path_abs = fs::absolute(out).string();
  const std::string path_q = sh_single_quote(path_abs);
  std::string command;
  if (command_exists_in_path("grim")) {
    command = "sh -lc \"grim " + path_q + "\"";
  } else if (command_exists_in_path("scrot")) {
    command = "sh -lc \"scrot " + path_q + "\"";
  } else {
    ctx.note = "no screenshot tool available (grim/scrot)";
    return ctx;
  }
  const CommandResult capture = run_command_capture(command, 30);
  if (!capture.ok || !fs::exists(out)) {
    const std::string msg = trim(capture.output);
    ctx.note = msg.empty() ? "screen capture failed" : msg;
    return ctx;
  }
  ctx.captured = true;
  ctx.image_path = fs::absolute(out);
#endif

  std::string ocr_note;
  if (ensure_tesseract_ocr(&ocr_note)) {
    ctx.ocr_text = trim(extract_ocr_text(ctx.image_path, 20));
  } else if (ctx.note.empty()) {
    ctx.note = ocr_note.empty() ? "tesseract OCR not available" : ocr_note;
  }
  return ctx;
}

inline std::string build_prompt_with_vision_context(const std::string& base_prompt, const ExternalVisionContext& vision) {
  if (!vision.requested) {
    return trim(base_prompt);
  }

  std::ostringstream out;
  out << trim(base_prompt);
  out << "\n\n[Vision context]\n";
  if (!vision.captured) {
    out << "Vision was requested, but screen capture failed";
    if (!vision.note.empty()) {
      out << ": " << vision.note;
    }
    out << ". Continue without image.\n";
    return trim(out.str());
  }

  out << "A screenshot was captured at: " << vision.image_path.string() << "\n";
  if (!vision.ocr_text.empty()) {
    out << "OCR text from the screenshot:\n" << vision.ocr_text << "\n";
  } else if (!vision.note.empty()) {
    out << "OCR note: " << vision.note << "\n";
  }
  out << "Use this visual context in your answer.\n";
  return trim(out.str());
}

inline std::vector<ExternalCliCommandCandidate> build_external_cli_commands(const ExternalCliRoute& route,
                                                                             const std::string& enriched_prompt,
                                                                             const ExternalVisionContext& vision) {
  const std::string prompt = shell_quote_arg(enriched_prompt);
  const std::string image = vision.captured ? shell_quote_arg(vision.image_path.string()) : "";

  if (route.name == "codex") {
    std::vector<ExternalCliCommandCandidate> cmds;
    if (vision.captured) {
      cmds.push_back({"codex exec --skip-git-repo-check --json -i " + image + " " + prompt, true});
    }
    cmds.push_back({"codex exec --skip-git-repo-check --json " + prompt, true});
    if (vision.captured) {
      cmds.push_back({"codex exec --skip-git-repo-check -i " + image + " " + prompt, false});
    }
    cmds.push_back({"codex exec --skip-git-repo-check " + prompt, false});
    return cmds;
  }

  std::vector<ExternalCliCommandCandidate> cmds;
  if (vision.captured) {
    cmds.push_back({"gemini -p " + prompt + " -i " + image, false});
    cmds.push_back({"gemini -p " + prompt + " --image " + image, false});
    cmds.push_back({"gemini -i " + image + " -p " + prompt, false});
  }
  cmds.push_back({"gemini -p " + prompt, false});
  cmds.push_back({"gemini " + prompt, false});
  return cmds;
}

inline std::string run_external_cli(const ExternalCliRoute& route, const fs::path& workspace, bool vision_enabled) {
  if (route.prompt.empty()) {
    return "Please include a prompt before " + route.suffix + ".";
  }

  if (vision_enabled && is_headless_server()) {
    return "Vision is unavailable on headless server (DISPLAY/WAYLAND_DISPLAY not set).";
  }

  std::string cli_note;
  if (route.name == "gemini") {
    if (!ensure_external_cli_available("gemini", "@google/gemini-cli", &cli_note)) {
      if (cli_note.empty()) {
        return "Gemini CLI is not installed. Install with: npm install -g @google/gemini-cli";
      }
      return "Gemini CLI install failed: " + cli_note;
    }
  } else {
    if (!ensure_external_cli_available("codex", "@openai/codex", &cli_note)) {
      if (cli_note.empty()) {
        return "Codex CLI is not installed. Install with: npm install -g @openai/codex";
      }
      return "Codex CLI install failed: " + cli_note;
    }
  }

  const ExternalVisionContext vision = collect_external_vision_context(vision_enabled);
  const std::string enriched_prompt = build_prompt_with_vision_context(route.prompt, vision);
  const auto commands = build_external_cli_commands(route, enriched_prompt, vision);

  CommandResult last;
  for (std::size_t i = 0; i < commands.size(); ++i) {
    last = run_command_capture(shell_in_dir_command(workspace, commands[i].command), 600);
    if (last.ok) {
      std::string extracted;
      if (route.name == "codex" && commands[i].expect_json) {
        extracted = extract_codex_json_message(last.output);
      }
      if (extracted.empty()) {
        extracted = extract_plain_cli_message(route.name, last.output);
      }
      return extracted.empty() ? (route.name + " completed with no output.") : extracted;
    }

    if (i + 1 < commands.size() && looks_like_cli_usage_error(last.output)) {
      continue;
    }
    break;
  }

  std::string error = extract_plain_cli_message(route.name, last.output);
  if (error.empty()) {
    error = trim(last.output);
  }
  if (error.empty()) {
    error = "Command failed with exit code " + std::to_string(last.exit_code) + ".";
  }
  return "Failed to run " + route.name + " for this request.\n" + error;
}

}  // namespace attoclaw
