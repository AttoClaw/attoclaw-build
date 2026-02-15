#pragma once

#include <functional>
#include <map>
#include <string>
#include <vector>
#include <unordered_map>

#include "attoclaw/common.hpp"
#include "attoclaw/http.hpp"

namespace attoclaw {

struct ToolCallRequest {
  std::string id;
  std::string name;
  json arguments{json::object()};
};

struct LLMResponse {
  std::string content;
  std::vector<ToolCallRequest> tool_calls;
  std::string finish_reason{"stop"};
  json usage{json::object()};
  std::string reasoning_content;

  bool has_tool_calls() const { return !tool_calls.empty(); }
};

class LLMProvider {
 public:
  virtual ~LLMProvider() = default;

  virtual LLMResponse chat(const json& messages, const json& tools, const std::string& model,
                           int max_tokens, double temperature, double top_p) = 0;

  // Optional streaming API. Default implementation calls chat() and emits the full content once.
  virtual LLMResponse chat_stream(const json& messages, const json& tools, const std::string& model, int max_tokens,
                                  double temperature, double top_p,
                                  const std::function<void(const std::string&)>& on_delta) {
    LLMResponse r = chat(messages, tools, model, max_tokens, temperature, top_p);
    if (on_delta && !r.content.empty()) {
      on_delta(r.content);
    }
    return r;
  }

  virtual std::string get_default_model() const = 0;
};

class OpenAICompatibleProvider : public LLMProvider {
 public:
  OpenAICompatibleProvider(std::string api_key, std::string api_base, std::string default_model)
      : api_key_(std::move(api_key)), api_base_(std::move(api_base)), default_model_(std::move(default_model)) {
    if (api_base_.empty()) {
      api_base_ = "https://openrouter.ai/api/v1";
    }
  }

  std::string get_default_model() const override { return default_model_; }

  LLMResponse chat(const json& messages, const json& tools, const std::string& model,
                   int max_tokens, double temperature, double top_p) override {
    LLMResponse out;
    if (api_key_.empty()) {
      out.content = "Error: no API key configured";
      out.finish_reason = "error";
      return out;
    }

    json payload = {{"model", model.empty() ? default_model_ : model},
                    {"messages", messages},
                    {"max_tokens", (std::max)(1, max_tokens)},
                    {"temperature", temperature},
                    {"top_p", top_p}};

    if (!tools.empty()) {
      payload["tools"] = tools;
      payload["tool_choice"] = "auto";
    }

    std::map<std::string, std::string> headers = {
        {"Authorization", "Bearer " + api_key_},
        {"Content-Type", "application/json"},
    };

    thread_local HttpClient client;
    HttpResponse resp = client.post(api_base_ + "/chat/completions", payload.dump(), headers, 90, true, 5);

    if (!resp.error.empty()) {
      out.content = "Error calling LLM: " + resp.error;
      out.finish_reason = "error";
      return out;
    }

    if (resp.status < 200 || resp.status >= 300) {
      out.content = "Error calling LLM (HTTP " + std::to_string(resp.status) + "): " + resp.body;
      out.finish_reason = "error";
      return out;
    }

    try {
      const json data = json::parse(resp.body);
      if (!data.contains("choices") || !data["choices"].is_array() || data["choices"].empty()) {
        out.content = "Error: malformed LLM response";
        out.finish_reason = "error";
        return out;
      }

      const json choice = data["choices"][0];
      out.finish_reason = choice.value("finish_reason", "stop");

      if (data.contains("usage") && data["usage"].is_object()) {
        out.usage = data["usage"];
      }

      if (!choice.contains("message") || !choice["message"].is_object()) {
        out.content = "Error: missing message in LLM response";
        out.finish_reason = "error";
        return out;
      }

      const json message = choice["message"];
      if (message.contains("content")) {
        if (message["content"].is_null()) {
          out.content.clear();
        } else if (message["content"].is_string()) {
          out.content = message["content"].get<std::string>();
        } else {
          out.content = message["content"].dump();
        }
      } else {
        out.content.clear();
      }

      if (message.contains("reasoning_content")) {
        if (message["reasoning_content"].is_null()) {
          out.reasoning_content.clear();
        } else if (message["reasoning_content"].is_string()) {
          out.reasoning_content = message["reasoning_content"].get<std::string>();
        } else {
          out.reasoning_content = message["reasoning_content"].dump();
        }
      } else {
        out.reasoning_content.clear();
      }

      if (message.contains("tool_calls") && message["tool_calls"].is_array()) {
        for (const auto& tc : message["tool_calls"]) {
          ToolCallRequest req;
          req.id = tc.value("id", random_id(10));
          if (tc.contains("function") && tc["function"].is_object()) {
            req.name = tc["function"].value("name", "");
            std::string arg_text = tc["function"].value("arguments", "{}");
            try {
              req.arguments = json::parse(arg_text);
            } catch (...) {
              req.arguments = json{{"raw", arg_text}};
            }
          }
          if (!req.name.empty()) {
            out.tool_calls.push_back(std::move(req));
          }
        }
      }

    } catch (const std::exception& e) {
      out.content = std::string("Error parsing LLM response: ") + e.what();
      out.finish_reason = "error";
    }

    return out;
  }

  LLMResponse chat_stream(const json& messages, const json& tools, const std::string& model, int max_tokens,
                          double temperature, double top_p,
                          const std::function<void(const std::string&)>& on_delta) override {
    LLMResponse out;
    if (api_key_.empty()) {
      out.content = "Error: no API key configured";
      out.finish_reason = "error";
      return out;
    }

    json payload = {{"model", model.empty() ? default_model_ : model},
                    {"messages", messages},
                    {"max_tokens", (std::max)(1, max_tokens)},
                    {"temperature", temperature},
                    {"top_p", top_p},
                    {"stream", true},
                    {"stream_options", {{"include_usage", true}}}};

    if (!tools.empty()) {
      payload["tools"] = tools;
      payload["tool_choice"] = "auto";
    }

    std::map<std::string, std::string> headers = {
        {"Authorization", "Bearer " + api_key_},
        {"Content-Type", "application/json"},
        {"Accept", "text/event-stream"},
    };

    std::string acc_content;
    std::string finish_reason;
    json usage = json::object();

    struct ToolCallAccum {
      std::string id;
      std::string name;
      std::string arguments_text;
    };
    std::unordered_map<int, ToolCallAccum> tool_calls;

    thread_local HttpClient client;
    bool done = false;
    HttpResponse resp = client.post_stream_lines(
        api_base_ + "/chat/completions", payload.dump(), headers,
        [&](const std::string& line) -> bool {
          if (done) {
            return false;
          }
          if (line.empty()) {
            return true;
          }
          if (line.rfind("data:", 0) != 0) {
            return true;
          }
          std::string data = trim(line.substr(5));
          if (data == "[DONE]") {
            done = true;
            return false;
          }

          try {
            const json evt = json::parse(data);
            if (evt.contains("usage") && evt["usage"].is_object()) {
              usage = evt["usage"];
            }
            if (!evt.contains("choices") || !evt["choices"].is_array() || evt["choices"].empty()) {
              return true;
            }
            const json choice = evt["choices"][0];
            const std::string fr = choice.value("finish_reason", "");
            if (!fr.empty()) {
              finish_reason = fr;
            }
            if (!choice.contains("delta") || !choice["delta"].is_object()) {
              return true;
            }
            const json delta = choice["delta"];
            if (delta.contains("content") && delta["content"].is_string()) {
              const std::string piece = delta["content"].get<std::string>();
              if (!piece.empty()) {
                acc_content += piece;
                if (on_delta) {
                  on_delta(piece);
                }
              }
            }

            if (delta.contains("tool_calls") && delta["tool_calls"].is_array()) {
              for (const auto& tc : delta["tool_calls"]) {
                const int index = tc.value("index", -1);
                if (index < 0) {
                  continue;
                }
                ToolCallAccum& a = tool_calls[index];
                if (tc.contains("id") && tc["id"].is_string() && a.id.empty()) {
                  a.id = tc["id"].get<std::string>();
                }
                if (tc.contains("function") && tc["function"].is_object()) {
                  const json fn = tc["function"];
                  if (fn.contains("name") && fn["name"].is_string() && a.name.empty()) {
                    a.name = fn["name"].get<std::string>();
                  }
                  if (fn.contains("arguments") && fn["arguments"].is_string()) {
                    a.arguments_text += fn["arguments"].get<std::string>();
                  }
                }
              }
            }
          } catch (...) {
            // Ignore malformed events.
          }
          return true;
        },
        180, true, 5);

    if (!resp.error.empty()) {
      out.content = "Error calling LLM (stream): " + resp.error;
      out.finish_reason = "error";
      return out;
    }
    if (resp.status < 200 || resp.status >= 300) {
      out.content = "Error calling LLM (stream) (HTTP " + std::to_string(resp.status) + ")";
      out.finish_reason = "error";
      return out;
    }

    out.content = acc_content;
    out.finish_reason = finish_reason.empty() ? "stop" : finish_reason;
    out.usage = usage;

    if (!tool_calls.empty()) {
      std::vector<std::pair<int, ToolCallAccum>> ordered;
      ordered.reserve(tool_calls.size());
      for (const auto& kv : tool_calls) {
        ordered.push_back(kv);
      }
      std::sort(ordered.begin(), ordered.end(),
                [](const auto& a, const auto& b) { return a.first < b.first; });
      for (const auto& kv : ordered) {
        ToolCallRequest req;
        req.id = kv.second.id.empty() ? random_id(10) : kv.second.id;
        req.name = kv.second.name;
        const std::string arg_text = kv.second.arguments_text.empty() ? "{}" : kv.second.arguments_text;
        try {
          req.arguments = json::parse(arg_text);
        } catch (...) {
          req.arguments = json{{"raw", arg_text}};
        }
        if (!req.name.empty()) {
          out.tool_calls.push_back(std::move(req));
        }
      }
    }

    return out;
  }

 private:
  std::string api_key_;
  std::string api_base_;
  std::string default_model_;
};

}  // namespace attoclaw
