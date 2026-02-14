#pragma once

#include <map>
#include <string>
#include <vector>

#include "attoclaw/common.hpp"

namespace attoclaw {

struct InboundMessage {
  std::string channel;
  std::string sender_id;
  std::string chat_id;
  std::string content;
  std::string timestamp{now_iso8601()};
  std::vector<std::string> media;
  json metadata{json::object()};

  std::string session_key() const { return channel + ":" + chat_id; }
};

struct OutboundMessage {
  std::string channel;
  std::string chat_id;
  std::string content;
  std::string reply_to;
  std::vector<std::string> media;
  json metadata{json::object()};
};

}  // namespace attoclaw

