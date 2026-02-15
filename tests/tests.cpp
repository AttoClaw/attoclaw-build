#include <cstdlib>
#include <iostream>
#include <sstream>

#include "attoclaw/config.hpp"
#include "attoclaw/external_cli.hpp"
#include "attoclaw/tools.hpp"
#include "attoclaw/vision.hpp"

static int fail(const std::string& msg, const char* file, int line) {
  std::cerr << "TEST FAIL: " << msg << " (" << file << ":" << line << ")\n";
  return 1;
}

#define EXPECT_TRUE(x)         \
  do {                         \
    if (!(x)) {                \
      return fail(#x, __FILE__, __LINE__); \
    }                          \
  } while (0)

#define EXPECT_EQ(a, b)                                              \
  do {                                                               \
    const auto _a = (a);                                             \
    const auto _b = (b);                                             \
    if (!(_a == _b)) {                                               \
      std::ostringstream ss;                                         \
      ss << #a << " == " << #b << " (got '" << _a << "' vs '" << _b << "')"; \
      return fail(ss.str(), __FILE__, __LINE__);                     \
    }                                                                \
  } while (0)

int main() {
  using namespace attoclaw;

  {
    const ParsedExternalRequest p = parse_external_request("hello --codex");
    EXPECT_TRUE(p.external_cli.has_value());
    EXPECT_EQ(p.external_cli->name, "codex");
    EXPECT_EQ(p.prompt, "hello");
  }

  {
    const ParsedExternalRequest p = parse_external_request("do thing --vision --gemini");
    EXPECT_TRUE(p.external_cli.has_value());
    EXPECT_EQ(p.external_cli->name, "gemini");
    EXPECT_TRUE(p.vision_enabled);
    EXPECT_EQ(p.prompt, "do thing");
  }

  {
    json root = default_config_json();
    root["channels"]["slack"]["enabled"] = true;
  #ifndef _WIN32
    setenv("SLACK_TOKEN_TEST", "xoxb-test", 1);
    root["channels"]["slack"]["token"] = "$SLACK_TOKEN_TEST";
  #else
    root["channels"]["slack"]["token"] = "xoxb-test";
  #endif
    root["channels"]["slack"]["channels"] = json::array({"C123"});

    root["tools"]["transcribe"]["apiKey"] = "k";
    root["tools"]["transcribe"]["apiBase"] = "https://api.example/v1";
    root["tools"]["transcribe"]["model"] = "whisper-1";

    const fs::path tmp = fs::temp_directory_path() / ("attoclaw_test_cfg_" + random_id(10) + ".json");
    EXPECT_TRUE(write_text_file(tmp, root.dump(2)));
    const Config cfg = load_config(tmp);
    std::error_code ec;
    fs::remove(tmp, ec);

    EXPECT_TRUE(cfg.channels.slack.enabled);
    EXPECT_EQ(cfg.channels.slack.token, "xoxb-test");
    EXPECT_EQ(cfg.channels.slack.channels.size(), static_cast<std::size_t>(1));
    EXPECT_EQ(cfg.channels.slack.channels[0], "C123");

    EXPECT_EQ(cfg.tools.transcribe.api_key, "k");
    EXPECT_EQ(cfg.tools.transcribe.api_base, "https://api.example/v1");
    EXPECT_EQ(cfg.tools.transcribe.model, "whisper-1");
  }

  {
    const auto parts = chunk_text(std::string(10, 'a'), 3);
    EXPECT_EQ(parts.size(), static_cast<std::size_t>(4));
    EXPECT_EQ(parts[0].size(), static_cast<std::size_t>(3));
    EXPECT_EQ(parts[3].size(), static_cast<std::size_t>(1));
  }

  {
    TranscribeTool t("", "https://api.example/v1", "whisper-1", 30);
    const std::string out = t.execute(json{{"path", "missing.wav"}});
    EXPECT_TRUE(out.find("apiKey") != std::string::npos);
  }

#ifndef _WIN32
  {
    setenv("DISPLAY", ":0", 1);
    EXPECT_TRUE(!is_headless_server());
    unsetenv("DISPLAY");
  }
#endif

  std::cout << "OK\n";
  return 0;
}
