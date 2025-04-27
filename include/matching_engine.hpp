#ifndef MATCHING_ENGINE_H
#define MATCHING_ENGINE_H

#include "logger.hpp"

#include <deque>
#include <map>
#include <unordered_map>
#include <string>
#include <vector>
#include <variant>

extern Logger logger;

enum class Side { Buy, Sell };

struct alignas(8) Order {
  uint64_t order_id;
  uint64_t quantity;
  double price;
  Side side;
};



class MatchingEngine {
public:
  MatchingEngine() = default;

  using token_t = std::vector<std::string>;
  using buy_book_t = std::map<double, std::deque<Order>, std::greater<double>>;
  using sell_book_t = std::map<double, std::deque<Order>>;
  using book_t = std::variant<buy_book_t, sell_book_t>;

  void processMessage(std::string const& message);

  bool messageToToken(std::string const& message, token_t& tokens);
  Order processTokens(token_t const& tokens, int& msg_type, int& sz);

  void addOrder(uint64_t order_id, uint64_t quantity, double price, Side side);
  void cancelOrder(uint64_t order_id);

private:

  void matchOrder(Order& aggressive_order);
  void addToBook(Order const& order);
  bool parseMessage(std::string const& message, std::vector<std::string>& tokens);

  void emitTradeEvent(uint64_t quantity, double price);
  void emitFullyFilled(uint64_t order_id);
  void emitPartiallyFilled(uint64_t order_id, uint64_t quantity);

  buy_book_t m_buyOrderBook;
  sell_book_t m_sellOrderBook;
  std::unordered_map<uint64_t, Order> m_orderMap;
};

#endif
