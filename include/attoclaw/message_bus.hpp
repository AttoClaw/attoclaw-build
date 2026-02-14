#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <semaphore>
#include <string>
#include <thread>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

#include "attoclaw/atomic_queue.hpp"
#include "attoclaw/common.hpp"
#include "attoclaw/events.hpp"

namespace attoclaw {

class MessageBus {
 public:
  using OutboundSubscriber = std::function<void(const OutboundMessage&)>;
  static constexpr std::size_t kInboundQueueCapacity = 1024;
  static constexpr std::size_t kOutboundQueueCapacity = 1024;

  MessageBus()
      : inbound_(std::make_unique<AtomicMPMCQueue<InboundMessage, kInboundQueueCapacity>>()),
        outbound_(std::make_unique<AtomicMPMCQueue<OutboundMessage, kOutboundQueueCapacity>>()) {}

  void publish_inbound(const InboundMessage& msg) {
    std::size_t spins = 0;
    while (!inbound_->try_push(msg)) {
      backoff(spins);
    }
    inbound_sem_.release();
  }

  InboundMessage consume_inbound() {
    inbound_sem_.acquire();
    InboundMessage msg;
    std::size_t spins = 0;
    while (!inbound_->try_pop(msg)) {
      backoff(spins);
    }
    return msg;
  }

  std::optional<InboundMessage> try_consume_inbound() {
    if (!inbound_sem_.try_acquire()) {
      return std::nullopt;
    }
    InboundMessage msg;
    std::size_t spins = 0;
    while (!inbound_->try_pop(msg)) {
      backoff(spins);
    }
    return msg;
  }

  void publish_outbound(const OutboundMessage& msg) {
    std::size_t spins = 0;
    while (!outbound_->try_push(msg)) {
      backoff(spins);
    }
    outbound_sem_.release();
  }

  OutboundMessage consume_outbound() {
    outbound_sem_.acquire();
    OutboundMessage msg;
    std::size_t spins = 0;
    while (!outbound_->try_pop(msg)) {
      backoff(spins);
    }
    return msg;
  }

  void subscribe_outbound(const std::string& channel, OutboundSubscriber cb) {
    std::lock_guard<std::mutex> lock(sub_mu_);
    outbound_subscribers_[channel].push_back(std::move(cb));
  }

  void start_dispatcher() {
    if (running_.exchange(true)) {
      return;
    }
    dispatcher_ = std::thread([this]() {
      while (running_.load()) {
        OutboundMessage msg = consume_outbound();
        if (!running_.load()) {
          break;
        }

        std::vector<OutboundSubscriber> subscribers;
        {
          std::lock_guard<std::mutex> lock(sub_mu_);
          auto it = outbound_subscribers_.find(msg.channel);
          if (it != outbound_subscribers_.end()) {
            subscribers = it->second;
          }
        }

        for (const auto& cb : subscribers) {
          try {
            cb(msg);
          } catch (const std::exception& e) {
            Logger::log(Logger::Level::kError,
                        "Outbound dispatch failed for channel " + msg.channel + ": " + e.what());
          }
        }
      }
    });
  }

  void stop_dispatcher() {
    if (!running_.exchange(false)) {
      return;
    }
    publish_outbound(OutboundMessage{});
    if (dispatcher_.joinable()) {
      dispatcher_.join();
    }
  }

 private:
  static void backoff(std::size_t& spins) {
    if (spins < 64) {
      ++spins;
      std::this_thread::yield();
      return;
    }
    std::this_thread::sleep_for(std::chrono::microseconds(100));
  }

  std::unique_ptr<AtomicMPMCQueue<InboundMessage, kInboundQueueCapacity>> inbound_;
  std::unique_ptr<AtomicMPMCQueue<OutboundMessage, kOutboundQueueCapacity>> outbound_;
  std::counting_semaphore<4096> inbound_sem_{0};
  std::counting_semaphore<4096> outbound_sem_{0};

  std::atomic<bool> running_{false};
  std::thread dispatcher_;

  std::mutex sub_mu_;
  std::unordered_map<std::string, std::vector<OutboundSubscriber>> outbound_subscribers_;
};

}  // namespace attoclaw

