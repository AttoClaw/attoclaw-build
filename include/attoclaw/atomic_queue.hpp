#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <optional>
#include <type_traits>

namespace attoclaw {

// Bounded lock-free MPMC queue (Vyukov algorithm).
template <typename T, std::size_t Capacity>
class AtomicMPMCQueue {
  static_assert((Capacity >= 2), "Capacity must be >= 2");

 public:
  AtomicMPMCQueue() {
    for (std::size_t i = 0; i < Capacity; ++i) {
      cells_[i].sequence.store(i, std::memory_order_relaxed);
    }
  }

  bool try_push(const T& value) {
    Cell* cell;
    std::size_t pos = enqueue_pos_.load(std::memory_order_relaxed);

    for (;;) {
      cell = &cells_[pos % Capacity];
      const std::size_t seq = cell->sequence.load(std::memory_order_acquire);
      const intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos);

      if (diff == 0) {
        if (enqueue_pos_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
          break;
        }
      } else if (diff < 0) {
        return false;
      } else {
        pos = enqueue_pos_.load(std::memory_order_relaxed);
      }
    }

    cell->data = value;
    cell->sequence.store(pos + 1, std::memory_order_release);
    return true;
  }

  bool try_push(T&& value) {
    Cell* cell;
    std::size_t pos = enqueue_pos_.load(std::memory_order_relaxed);

    for (;;) {
      cell = &cells_[pos % Capacity];
      const std::size_t seq = cell->sequence.load(std::memory_order_acquire);
      const intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos);

      if (diff == 0) {
        if (enqueue_pos_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
          break;
        }
      } else if (diff < 0) {
        return false;
      } else {
        pos = enqueue_pos_.load(std::memory_order_relaxed);
      }
    }

    cell->data = std::move(value);
    cell->sequence.store(pos + 1, std::memory_order_release);
    return true;
  }

  bool try_pop(T& out) {
    Cell* cell;
    std::size_t pos = dequeue_pos_.load(std::memory_order_relaxed);

    for (;;) {
      cell = &cells_[pos % Capacity];
      const std::size_t seq = cell->sequence.load(std::memory_order_acquire);
      const intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos + 1);

      if (diff == 0) {
        if (dequeue_pos_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
          break;
        }
      } else if (diff < 0) {
        return false;
      } else {
        pos = dequeue_pos_.load(std::memory_order_relaxed);
      }
    }

    out = std::move(cell->data);
    cell->sequence.store(pos + Capacity, std::memory_order_release);
    return true;
  }

 private:
  struct Cell {
    std::atomic<std::size_t> sequence;
    T data;
  };

  std::array<Cell, Capacity> cells_{};
  alignas(64) std::atomic<std::size_t> enqueue_pos_{0};
  alignas(64) std::atomic<std::size_t> dequeue_pos_{0};
};

}  // namespace attoclaw

