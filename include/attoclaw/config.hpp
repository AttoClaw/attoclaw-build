#pragma once

#include <optional>
#include <string>
#include <vector>

#include "attoclaw/common.hpp"

namespace attoclaw {

struct ProviderConfig {
  std::string api_key;
  std::string api_base;
};

struct AgentDefaults {
  std::string workspace{"~/.attoclaw/workspace"};
  std::string model{"openai/gpt-4o-mini"};
  int max_tokens{2048};
  double temperature{0.7};
  double top_p{0.9};
  int max_tool_iterations{10};
  int memory_window{24};
};

struct ExecConfig {
  int timeout{60};
};

struct WebSearchConfig {
  std::string api_key;
  int max_results{5};
};

struct ToolsConfig {
  ExecConfig exec{};
  WebSearchConfig web_search{};
  bool restrict_to_workspace{false};
};

struct BasicChannelConfig {
  bool enabled{false};
};

struct TelegramChannelConfig : BasicChannelConfig {
  std::string token;
  std::vector<std::string> allow_from;
  std::string proxy;
};

struct WhatsAppChannelConfig : BasicChannelConfig {
  std::string bridge_url{"ws://localhost:3001"};
  std::string bridge_token;
  std::vector<std::string> allow_from;
};

struct ChannelsConfig {
  WhatsAppChannelConfig whatsapp{};
  TelegramChannelConfig telegram{};
};

struct Config {
  AgentDefaults agent{};
  ProviderConfig provider{};
  ToolsConfig tools{};
  ChannelsConfig channels{};
};

inline std::string to_lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

inline std::string default_api_base_for_provider(const std::string& provider_key) {
  const std::string key = to_lower(provider_key);
  if (key == "openrouter") {
    return "https://openrouter.ai/api/v1";
  }
  if (key == "openai") {
    return "https://api.openai.com/v1";
  }
  if (key == "nim" || key == "nvidia") {
    return "https://integrate.api.nvidia.com/v1";
  }
  return "";
}

inline std::string default_api_key_env_for_provider(const std::string& provider_key) {
  const std::string key = to_lower(provider_key);
  if (key == "openrouter") {
    return "OPENROUTER_API_KEY";
  }
  if (key == "openai") {
    return "OPENAI_API_KEY";
  }
  if (key == "nim" || key == "nvidia") {
    return "NVIDIA_API_KEY";
  }
  return "";
}

inline std::string resolve_env_ref(const std::string& value) {
  if (value.empty()) {
    return "";
  }

  // Supports "$ENV_NAME" and "${ENV_NAME}".
  if (value[0] != '$') {
    return value;
  }

  std::string env_name = value.substr(1);
  if (!env_name.empty() && env_name.front() == '{' && env_name.back() == '}') {
    env_name = env_name.substr(1, env_name.size() - 2);
  }
  if (env_name.empty()) {
    return value;
  }

  const char* v = std::getenv(env_name.c_str());
  return (v && *v) ? std::string(v) : "";
}

inline fs::path get_data_dir() {
  return expand_user_path("~/.attoclaw");
}

inline fs::path get_config_path() {
  return get_data_dir() / "config.json";
}

inline json default_config_json() {
  return json{
      {"providers",
       {
           {"openrouter", {{"apiKey", ""}, {"apiBase", "https://openrouter.ai/api/v1"}}},
           {"openai", {{"apiKey", ""}, {"apiBase", "https://api.openai.com/v1"}}},
           {"nim", {{"apiKey", ""}, {"apiBase", "https://integrate.api.nvidia.com/v1"}}},
       }},
      {"agents",
       {
           {"defaults",
            {
                {"workspace", "~/.attoclaw/workspace"},
                {"model", "openai/gpt-4o-mini"},
                {"maxTokens", 2048},
                {"temperature", 0.7},
                {"topP", 0.9},
                {"maxToolIterations", 10},
                {"memoryWindow", 24},
            }},
       }},
      {"tools",
       {
           {"exec", {{"timeout", 60}}},
           {"web", {{"search", {{"apiKey", ""}, {"maxResults", 5}}}}},
           {"restrictToWorkspace", false},
       }},
      {"channels",
       {
           {"whatsapp",
            {{"enabled", false}, {"bridgeUrl", "ws://localhost:3001"}, {"bridgeToken", ""}, {"allowFrom", json::array()}}},
           {"telegram", {{"enabled", false}, {"token", ""}, {"allowFrom", json::array()}, {"proxy", ""}}},
       }}};
}

inline std::optional<ProviderConfig> extract_provider(const json& root, const std::string& model_hint) {
  if (!root.contains("providers") || !root["providers"].is_object()) {
    return std::nullopt;
  }

  const auto& providers = root["providers"];

  auto pick = [&](const std::string& key) -> std::optional<ProviderConfig> {
    if (!providers.contains(key) || !providers[key].is_object()) {
      return std::nullopt;
    }
    const auto& p = providers[key];
    ProviderConfig out;
    out.api_key = resolve_env_ref(p.value("apiKey", ""));
    if (out.api_key.empty()) {
      const std::string env_name = default_api_key_env_for_provider(key);
      if (!env_name.empty()) {
        const char* env_val = std::getenv(env_name.c_str());
        if (env_val && *env_val) {
          out.api_key = std::string(env_val);
        }
      }
    }
    out.api_base = p.value("apiBase", "");
    if (out.api_base.empty()) {
      out.api_base = default_api_base_for_provider(key);
    }
    if (out.api_key.empty()) {
      return std::nullopt;
    }
    return out;
  };

  const std::string m = to_lower(model_hint);
  if (m.find("openrouter") != std::string::npos) {
    if (auto p = pick("openrouter")) {
      return p;
    }
  }
  if (m.find("openai") != std::string::npos) {
    if (auto p = pick("openai")) {
      return p;
    }
  }
  if (m.find("nvidia") != std::string::npos || m.find("nim") != std::string::npos) {
    if (auto p = pick("nim")) {
      return p;
    }
    if (auto p = pick("nvidia")) {
      return p;
    }
  }

  for (auto it = providers.begin(); it != providers.end(); ++it) {
    if (!it.value().is_object()) {
      continue;
    }
    ProviderConfig p;
    p.api_key = resolve_env_ref(it.value().value("apiKey", ""));
    if (p.api_key.empty()) {
      const std::string env_name = default_api_key_env_for_provider(it.key());
      if (!env_name.empty()) {
        const char* env_val = std::getenv(env_name.c_str());
        if (env_val && *env_val) {
          p.api_key = std::string(env_val);
        }
      }
    }
    p.api_base = it.value().value("apiBase", "");
    if (p.api_base.empty()) {
      p.api_base = default_api_base_for_provider(it.key());
    }
    if (!p.api_key.empty()) {
      return p;
    }
  }

  return std::nullopt;
}

inline Config load_config(const fs::path& path = get_config_path()) {
  Config cfg{};
  fs::path resolved = path;
  const std::string raw = read_text_file(resolved);
  if (raw.empty()) {
    return cfg;
  }

  try {
    const json root = json::parse(raw);

    if (root.contains("agents") && root["agents"].is_object()) {
      const auto& agents = root["agents"];
      if (agents.contains("defaults") && agents["defaults"].is_object()) {
        const auto& d = agents["defaults"];
        cfg.agent.workspace = d.value("workspace", cfg.agent.workspace);
        cfg.agent.model = d.value("model", cfg.agent.model);
        cfg.agent.max_tokens = d.value("maxTokens", cfg.agent.max_tokens);
        cfg.agent.temperature = d.value("temperature", cfg.agent.temperature);
        cfg.agent.top_p = d.value("topP", cfg.agent.top_p);
        cfg.agent.max_tool_iterations = d.value("maxToolIterations", cfg.agent.max_tool_iterations);
        cfg.agent.memory_window = d.value("memoryWindow", cfg.agent.memory_window);
      }
    }

    if (auto provider = extract_provider(root, cfg.agent.model)) {
      cfg.provider = *provider;
    }

    if (root.contains("tools") && root["tools"].is_object()) {
      const auto& tools = root["tools"];
      cfg.tools.restrict_to_workspace = tools.value("restrictToWorkspace", false);
      if (tools.contains("exec") && tools["exec"].is_object()) {
        cfg.tools.exec.timeout = tools["exec"].value("timeout", cfg.tools.exec.timeout);
      }
      if (tools.contains("web") && tools["web"].is_object()) {
        const auto& web = tools["web"];
        if (web.contains("search") && web["search"].is_object()) {
          cfg.tools.web_search.api_key = web["search"].value("apiKey", "");
          cfg.tools.web_search.max_results = web["search"].value("maxResults", cfg.tools.web_search.max_results);
        }
      }
    }

    if (root.contains("channels") && root["channels"].is_object()) {
      const auto& channels = root["channels"];

      if (channels.contains("whatsapp") && channels["whatsapp"].is_object()) {
        const auto& wa = channels["whatsapp"];
        cfg.channels.whatsapp.enabled = wa.value("enabled", cfg.channels.whatsapp.enabled);
        cfg.channels.whatsapp.bridge_url = wa.value("bridgeUrl", cfg.channels.whatsapp.bridge_url);
        cfg.channels.whatsapp.bridge_token = wa.value("bridgeToken", cfg.channels.whatsapp.bridge_token);
        if (wa.contains("allowFrom") && wa["allowFrom"].is_array()) {
          cfg.channels.whatsapp.allow_from.clear();
          for (const auto& item : wa["allowFrom"]) {
            if (item.is_string()) {
              cfg.channels.whatsapp.allow_from.push_back(item.get<std::string>());
            } else if (item.is_number_integer()) {
              cfg.channels.whatsapp.allow_from.push_back(std::to_string(item.get<long long>()));
            }
          }
        }
      }

      if (channels.contains("telegram") && channels["telegram"].is_object()) {
        const auto& tg = channels["telegram"];
        cfg.channels.telegram.enabled = tg.value("enabled", cfg.channels.telegram.enabled);
        cfg.channels.telegram.token = tg.value("token", cfg.channels.telegram.token);
        cfg.channels.telegram.proxy = tg.value("proxy", cfg.channels.telegram.proxy);
        if (tg.contains("allowFrom") && tg["allowFrom"].is_array()) {
          cfg.channels.telegram.allow_from.clear();
          for (const auto& item : tg["allowFrom"]) {
            if (item.is_string()) {
              cfg.channels.telegram.allow_from.push_back(item.get<std::string>());
            } else if (item.is_number_integer()) {
              cfg.channels.telegram.allow_from.push_back(std::to_string(item.get<long long>()));
            }
          }
        }
      }
    }

  } catch (const std::exception& e) {
    Logger::log(Logger::Level::kWarn, std::string("Failed to parse config: ") + e.what());
  }

  return cfg;
}

inline bool save_default_config(const fs::path& path = get_config_path()) {
  std::error_code ec;
  fs::create_directories(path.parent_path(), ec);
  std::ofstream out(path, std::ios::out | std::ios::binary | std::ios::trunc);
  if (!out) {
    return false;
  }
  out << default_config_json().dump(2);
  return true;
}

}  // namespace attoclaw

