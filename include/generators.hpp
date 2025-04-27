#ifndef MESSAGE_GENERATOR
#define MESSAGE_GENERATOR

#include "matching_engine.hpp"
#include "thread_pool.hpp"
#include "spsc_queue.hpp"

#include <iostream>
#include <mutex>
#include <queue>
#include <string>
#include <tuple>
#include <vector>
#include <random>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <csignal>
#include <chrono>
#include <thread>
#include <poll.h>
#include <unistd.h>

static std::atomic<bool> shutdown{ false };

void signalHandler(int signum) {
    std::cout << "Interrupt signal (" << signum << ") received in thread " << std::this_thread::get_id() << "\n";
    shutdown.store(true, std::memory_order_relaxed);
}

SPSCQueue<std::string, 10000> msg_q;
SPSCQueue<MatchingEngine::token_t, 10000> token_q;
SPSCQueue<std::pair<Order, int>, 10000> order_q;

namespace {
  std::vector<std::string> generate_test_case(size_t num_messages = 100) {
    std::vector<std::string> inputs;
    inputs.reserve(num_messages);
    std::random_device rd;
    std::mt19937_64 rng(rd());
    std::uniform_int_distribution<int> side_dist(0, 1);
    std::uniform_int_distribution<int> quantity_dist(1, 10000);
    std::uniform_real_distribution<double> price_dist(90, 130.00);
    std::uniform_int_distribution<int> message_type_dist(0, 99);
    std::uniform_int_distribution<uint64_t> order_id_dist(1000000, 1000000000);

    std::vector<uint64_t> active_order_ids;
    active_order_ids.reserve(num_messages);

    for (size_t i = 0; i < num_messages; ++i) {
      std::ostringstream oss;
      int message_type = message_type_dist(rng);
      if (message_type < 90) {
        uint64_t order_id;
        do {
          order_id = order_id_dist(rng);
        } while (std::find(active_order_ids.begin(), active_order_ids.end(), order_id) != active_order_ids.end());
        active_order_ids.push_back(order_id);
        int side = side_dist(rng);
        int quantity = quantity_dist(rng);
        double price = std::round(price_dist(rng) * 100) / 100.0;
        oss << 0 << "," << order_id << "," << side << "," << quantity << "," << std::fixed << std::setprecision(2) << price;
      }
      else if (message_type < 99 && !active_order_ids.empty()) {
        std::uniform_int_distribution<size_t> cancel_idx_dist(0, active_order_ids.size() - 1);
        size_t cancel_idx = cancel_idx_dist(rng);
        uint64_t order_id = active_order_ids[cancel_idx];
        active_order_ids.erase(active_order_ids.begin() + cancel_idx);
        oss << 1 << "," << order_id;
      }
      else {
        int invalid_type = message_type % 5;
        uint64_t order_id = order_id_dist(rng);
        if (invalid_type == 0) {
          oss << "BADMESSAGE";
        }
        else if (invalid_type == 1) {
          oss << 0 << "," << order_id << ",2,10,1000.00";
        }
        else if (invalid_type == 2) {
          oss << 0 << "," << order_id << ",0,-5,1000.00";
        }
        else if (invalid_type == 3) {
          oss << 0 << "," << order_id << ",0,5,-1000.00";
        }
        else {
          oss << 0 << ",abc,0,10,1000.00";
        }
      }
      inputs.push_back(oss.str());
    }
    return inputs;
  }
}

void messageGeneratorForTest() {
  while (!shutdown) {
    if (msg_q.size() > 5000) { // Throttle if queue is half full
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      continue;
    }
    std::vector<std::string> messages = generate_test_case(100);
    for (auto& msg : messages) {
      if (shutdown) break;
      if (!msg_q.wait_push_timeout(std::move(msg), std::chrono::milliseconds(50))) {
        std::cerr << "messageGeneratorForTest: Queue full, size=" << msg_q.size() << "\n";
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    }
  }
  std::cout << "messageGeneratorForTest exiting\n";
}

void messageFromConsole() {
  MatchingEngine engine;
  std::string line;
  while (!shutdown) {
    struct pollfd pfd = { STDIN_FILENO, POLLIN, 0 };
    int ret = poll(&pfd, 1, 500);
    if (ret > 0 && (pfd.revents & POLLIN)) {
      if (std::getline(std::cin, line) && !line.empty()) {
        if (!msg_q.push(std::move(line))) {
          std::cerr << "msg_q full, size=" << msg_q.size() << "\n";
          std::this_thread::yield();
        }
        continue;
      }
    } else if (ret == -1) {
      if (shutdown) break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  msg_q.push("DUMMY");
  std::cout << "messageFromConsole exiting\n";
}

void tokensGenerator() {
  MatchingEngine engine;
  MatchingEngine::token_t tokens;
  std::string message;
  while (!shutdown || msg_q.size() > 0) {
    if (!msg_q.wait_pop_timeout(message, std::chrono::milliseconds(100))) {
      continue;
    }
    if (message == "DUMMY") break;
    try {
      bool finished = engine.messageToToken(message, tokens);
      if (!finished) {
        std::cerr << "Error processing message: " << message << "\n";
        tokens.clear();
      } else {
        if (!token_q.push(std::move(tokens))) {
          std::cerr << "token_q full, size=" << token_q.size() << "\n";
          std::this_thread::yield();
        }
      }
    } catch (const std::exception& e) {
      std::cerr << "Exception in tokensGenerator: " << e.what() << " for message: " << message << "\n";
    }
  }
  token_q.push(MatchingEngine::token_t{});
  std::cout << "tokensGenerator exiting\n";
}

void ordersGenerator() {
  MatchingEngine engine;
  MatchingEngine::token_t tokens;
  Order order;
  while (!shutdown || token_q.size() > 0) {
    if (!token_q.wait_pop_timeout(tokens, std::chrono::milliseconds(100))) {
      continue;
    }
    if (tokens.empty()) break;
    int msg_type = -1;
    int sz = -1;
    order = engine.processTokens(tokens, msg_type, sz);
    if (sz > 0) {
      if (!order_q.push(std::make_pair(std::move(order), msg_type))) {
        // std::cerr << "order_q full, size=" << order_q.size() << "\n";
        std::this_thread::yield();
      }
    }
  }
  order_q.push(std::make_pair(Order{}, -1));
  std::cout << "ordersGenerator exiting\n";
}

void orderProcessor() {
  thread_local MatchingEngine engine;
  std::pair<Order, int> order_info;
  while (!shutdown || order_q.size() > 0) {
    if (!order_q.wait_pop_timeout(order_info, std::chrono::milliseconds(100))) {
      continue;
    }
    if (order_info.second == -1) break;
    if (order_info.second == 0) {
      engine.addOrder(order_info.first.order_id, order_info.first.quantity, order_info.first.price, order_info.first.side);
    } else if (order_info.second == 1) {
      engine.cancelOrder(order_info.first.order_id);
    }
  }
  std::cout << "orderProcessor exiting\n";
}

void matching_engine_mt() {
  struct sigaction sa;
  sa.sa_handler = signalHandler;
  sa.sa_flags = SA_RESTART;
  sigemptyset(&sa.sa_mask);
  if (sigaction(SIGINT, &sa, nullptr) == -1) {
    std::cerr << "Failed to set SIGINT handler\n";
    return;
  }

  ThreadPool threadPool{ std::thread::hardware_concurrency() };
  
  
  // threadPool.push(messageGeneratorForTest); // TEST ONLY
  
  
  threadPool.push(messageFromConsole);
  threadPool.push(tokensGenerator);
  threadPool.push(ordersGenerator);
  threadPool.push(orderProcessor);
  while (!shutdown) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  msg_q.push("DUMMY");
  token_q.push(MatchingEngine::token_t{});
  order_q.push(std::make_pair(Order{}, -1));
  threadPool.stop();
  std::cout << "All threads cleaned up. Exiting.\n";
}

#endif