#ifndef LOGGER_HPP
#define LOGGER_HPP

#include "spsc_queue.hpp"
#include <iostream>
#include <string>
#include <thread>
#include <atomic>

class Logger {
public:
  Logger() {
    thread_ = std::thread(&Logger::run, this);
  }

  ~Logger() {
    shutdown_.store(true, std::memory_order_release);
    shutdown_.notify_one();
    thread_.join();
  }

  void set_out_stream(std::ostream* out_stream, std::ostream* err_stream) {
    out_stream_.store(out_stream ? out_stream : &std::cout, std::memory_order_release);
    err_stream_.store(err_stream ? err_stream : &std::cerr, std::memory_order_release);
  }

  void set_enabled(bool enabled) {
    enabled_.store(enabled, std::memory_order_release);
  }

  void log_out(const std::string& msg) {
    if (!enabled_.load(std::memory_order_acquire)) {
      *out_stream_.load(std::memory_order_acquire) << msg << std::endl;
      return;
    }
    if (!queue_.push("OUT: " + msg)) {
      std::this_thread::yield();
    }
  }

  void log_err(const std::string& msg) {
    if (!enabled_.load(std::memory_order_acquire)) {
      *err_stream_.load(std::memory_order_acquire) << msg << std::endl;
      return;
    }
    if (!queue_.push("ERR: " + msg)) {
      std::this_thread::yield();
    }
  }

private:
  void run() {
    while (true) {
      std::string msg;
      if (!queue_.pop(msg)) {
        if (shutdown_.load(std::memory_order_acquire)) {
          break;
        }
        continue;
      }
      std::ostream* stream = (msg.find("ERR: ") == 0) ? err_stream_.load(std::memory_order_acquire) : out_stream_.load(std::memory_order_acquire);
      *stream << msg.substr(5) << std::endl;
    }
  }

  SPSCQueue<std::string, 1000> queue_;
  std::thread thread_;
  std::atomic<bool> shutdown_{ false };
  std::atomic<bool> enabled_{ true };
  std::atomic<std::ostream*> out_stream_{ &std::cout };
  std::atomic<std::ostream*> err_stream_{ &std::cerr };
};

#endif // LOGGER_HPP