#pragma once

#include <string>
#include <vector>

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

    HttpClient client;
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

 private:
  std::string api_key_;
  std::string api_base_;
  std::string default_model_;
};

}  // namespace attoclaw

