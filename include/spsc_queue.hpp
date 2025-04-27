#ifndef SPSC_QUEUE_HPP
#define SPSC_QUEUE_HPP

#include <atomic>
#include <memory>
#include <stdexcept>
#include <array>
#include <chrono>
#include <thread>

template <typename T, size_t Capacity>
class SPSCQueue {
public:
  SPSCQueue() : head_(0), tail_(0) {}

  bool push(const T& value) {
    auto head = head_.load(std::memory_order_relaxed);
    auto nextHead = (head + 1) % Capacity;
    if (nextHead == tail_.load(std::memory_order_acquire)) {
      return false; // Queue is full
    }
    buffer_[head] = value;
    head_.store(nextHead, std::memory_order_release);
    head_.notify_one();
    return true;
  }

  bool push(T&& value) {
    auto head = head_.load(std::memory_order_relaxed);
    auto nextHead = (head + 1) % Capacity;
    if (nextHead == tail_.load(std::memory_order_acquire)) {
      return false; // Queue is full
    }
    buffer_[head] = std::move(value);
    head_.store(nextHead, std::memory_order_release);
    head_.notify_one();
    return true;
  }

  bool wait_push_timeout(T&& value, const std::chrono::milliseconds& timeout) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    auto head = head_.load(std::memory_order_relaxed);
    auto nextHead = (head + 1) % Capacity;
    while (nextHead == tail_.load(std::memory_order_acquire)) {
      if (std::chrono::steady_clock::now() >= deadline) {
        return false;
      }
      head_.wait(head, std::memory_order_acquire);
      head = head_.load(std::memory_order_relaxed);
      nextHead = (head + 1) % Capacity;
      if (nextHead == tail_.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::microseconds(100));
      }
    }
    buffer_[head] = std::move(value);
    head_.store(nextHead, std::memory_order_release);
    head_.notify_one();
    return true;
  }

  bool pop(T& value) {
    auto tail = tail_.load(std::memory_order_relaxed);
    auto head = head_.load(std::memory_order_acquire);
    if (tail == head) {
      return false; // Queue is empty
    }
    value = std::move(buffer_[tail]);
    buffer_[tail] = T{};
    tail_.store((tail + 1) % Capacity, std::memory_order_release);
    head_.notify_one();
    return true;
  }

  bool wait_pop(T& value) {
    auto tail = tail_.load(std::memory_order_relaxed);
    auto head = head_.load(std::memory_order_acquire);
    while (tail == head) {
      head_.wait(head, std::memory_order_acquire);
      tail = tail_.load(std::memory_order_relaxed);
      head = head_.load(std::memory_order_acquire);
    }
    value = std::move(buffer_[tail]);
    buffer_[tail] = T{};
    tail_.store((tail + 1) % Capacity, std::memory_order_release);
    head_.notify_one();
    return true;
  }

  bool wait_pop_timeout(T& value, const std::chrono::milliseconds& timeout) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    auto tail = tail_.load(std::memory_order_relaxed);
    auto head = head_.load(std::memory_order_acquire);
    while (tail == head) {
      if (std::chrono::steady_clock::now() >= deadline) {
        return false;
      }
      head_.wait(head, std::memory_order_acquire);
      tail = tail_.load(std::memory_order_relaxed);
      head = head_.load(std::memory_order_acquire);
      if (tail == head) {
        std::this_thread::sleep_for(std::chrono::microseconds(100));
      }
    }
    value = std::move(buffer_[tail]);
    buffer_[tail] = T{};
    tail_.store((tail + 1) % Capacity, std::memory_order_release);
    head_.notify_one();
    return true;
  }

  bool empty() const {
    return head_.load(std::memory_order_acquire) == tail_.load(std::memory_order_acquire);
  }

  bool full() const {
    auto head = head_.load(std::memory_order_acquire);
    auto nextHead = (head + 1) % Capacity;
    return nextHead == tail_.load(std::memory_order_acquire);
  }

  size_t size() const {
    auto head = head_.load(std::memory_order_acquire);
    auto tail = tail_.load(std::memory_order_acquire);
    return (head >= tail) ? (head - tail) : (Capacity - tail + head);
  }

private:
  std::array<T, Capacity> buffer_;
  alignas(64) std::atomic<size_t> head_;
  alignas(64) std::atomic<size_t> tail_;
};

#endif