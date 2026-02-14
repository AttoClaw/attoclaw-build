#pragma once

#include <memory>
#include <string>
#include <vector>

#include "attoclaw/events.hpp"
#include "attoclaw/message_bus.hpp"

namespace attoclaw {

class BaseChannel {
 public:
  virtual ~BaseChannel() = default;

  explicit BaseChannel(std::string name, MessageBus* bus) : name_(std::move(name)), bus_(bus) {}

  virtual void start() = 0;
  virtual void stop() = 0;
  virtual void send(const OutboundMessage& msg) = 0;

  const std::string& name() const { return name_; }

 protected:
  void handle_message(const std::string& sender_id, const std::string& chat_id, const std::string& content) {
    if (!bus_) {
      return;
    }
    bus_->publish_inbound(InboundMessage{name_, sender_id, chat_id, content});
  }

  std::string name_;
  MessageBus* bus_;
};

class ChannelManager {
 public:
  explicit ChannelManager(MessageBus* bus) : bus_(bus) {}

  void add_channel(std::shared_ptr<BaseChannel> channel) {
    channels_.push_back(channel);
    bus_->subscribe_outbound(channel->name(), [channel](const OutboundMessage& msg) { channel->send(msg); });
  }

  void start_all() {
    for (auto& c : channels_) {
      c->start();
    }
  }

  void stop_all() {
    for (auto& c : channels_) {
      c->stop();
    }
  }

  std::vector<std::string> enabled_channels() const {
    std::vector<std::string> out;
    for (const auto& c : channels_) {
      out.push_back(c->name());
    }
    return out;
  }

 private:
  MessageBus* bus_;
  std::vector<std::shared_ptr<BaseChannel>> channels_;
};

}  // namespace attoclaw

