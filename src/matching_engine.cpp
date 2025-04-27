#include "matching_engine.hpp"
#include "logger.hpp"

#include <iostream>
#include <sstream>
#include <algorithm>

// Global logger instance
Logger logger;

bool MatchingEngine::messageToToken(std::string const& message, token_t& tokens)
{
  if (!parseMessage(message, tokens)) {
    std::stringstream ss;
    ss << "Unknown message: " << message;
    logger.log_err(ss.str());
    return false;
  }
  return true;
}

Order MatchingEngine::processTokens(token_t const& tokens, int& msg_type, int& sz)
{
  Order order;
  try {
    msg_type = std::stoi(tokens[0]);
    if (msg_type == 0 && tokens.size() == 5) {
      uint64_t order_id = std::stoull(tokens[1]);
      Side side = (tokens[2] == "0") ? Side::Buy : Side::Sell;
      if (tokens[3][0] == '-') {
        std::stringstream ss;
        ss << "Invalid order: quantity is negative.";
        logger.log_err(ss.str());
        return order;
      }
      uint64_t quantity = std::stoull(tokens[3]);
      double price = std::stod(tokens[4]);
      if (quantity == 0 || price <= 0) {
        std::stringstream ss;
        ss << "Invalid order: quantity=" << quantity << ", price=" << price;
        logger.log_err(ss.str());
        return order;
      }
      sz = static_cast<int>(tokens.size());
      order.order_id = order_id;
      order.side = side;
      order.quantity = quantity;
      order.price = price;
    }
    else if (msg_type == 1 && tokens.size() == 2) {
      uint64_t order_id = std::stoull(tokens[1]);
      sz = static_cast<int>(tokens.size());
      order.order_id = order_id;
    }
    else {
      std::stringstream ss;
      ss << "Invalid message format";
      logger.log_err(ss.str());
      return order;
    }
  }
  catch (const std::exception& e) {
    std::stringstream ss;
    ss << "Error processing message" << " (" << e.what() << ")";
    logger.log_err(ss.str());
    return order;
  }
  return order;
}

void MatchingEngine::processMessage(std::string const& message) {
  std::vector<std::string> tokens;
  if (!messageToToken(message, tokens))
  {
    return;
  }

  int sz = -1;
  int msg_type = -1;

  Order order = processTokens(tokens, msg_type, sz);

  if (msg_type == 0 && sz > 0)
  {
    addOrder(order.order_id, order.quantity, order.price, order.side);
  }
  else if (msg_type == 1 && sz > 0)
  {
    cancelOrder(order.order_id);
  }

}

bool MatchingEngine::parseMessage(std::string const& message, std::vector<std::string>& tokens) {
  std::stringstream ss(message);
  std::string token;
  while (std::getline(ss, token, ',')) {
    tokens.push_back(token);
  }
  return !tokens.empty();
}

void MatchingEngine::addOrder(uint64_t order_id, uint64_t quantity, double price, Side side) {
  if (m_orderMap.find(order_id) != m_orderMap.end()) {
    std::stringstream ss;
    ss << "Duplicate order ID: " << order_id;
    logger.log_err(ss.str());
    return;
  }

  Order order{ order_id, quantity, price, side };
  matchOrder(order);
  if (order.quantity > 0) {
    addToBook(order);
  }
}

void MatchingEngine::cancelOrder(uint64_t order_id) {
  auto it = m_orderMap.find(order_id);
  if (it == m_orderMap.end()) {
    std::stringstream ss;
    ss << "Order not found: " << order_id;
    logger.log_err(ss.str());
    return;
  }

  const Order& order = it->second;

  if (order.side == Side::Buy)
  {
    auto& book = m_buyOrderBook;
    auto price_it = book.find(order.price);
    if (price_it != book.end()) {
      auto& orders = price_it->second;
      orders.erase(std::remove_if(orders.begin(), orders.end(),
        [order_id](const Order& o) { return o.order_id == order_id; }),
        orders.end());
      if (orders.empty()) {
        book.erase(price_it);
      }
    }
    m_orderMap.erase(it);
  }
  else {
    auto& book = m_sellOrderBook;
    auto price_it = book.find(order.price);
    if (price_it != book.end()) {
      auto& orders = price_it->second;
      orders.erase(std::remove_if(orders.begin(), orders.end(),
        [order_id](const Order& o) { return o.order_id == order_id; }),
        orders.end());
      if (orders.empty()) {
        book.erase(price_it);
      }
    }
    m_orderMap.erase(it);
  }
}

void MatchingEngine::matchOrder(Order& aggressive_order) {

  if (aggressive_order.side == Side::Buy)
  {
    auto& opposite_book = m_sellOrderBook;
    while (aggressive_order.quantity > 0 && !opposite_book.empty()) {
      auto best_price_it = opposite_book.begin();
      double best_price = best_price_it->first;
      bool can_match = (aggressive_order.side == Side::Buy) ?
        (aggressive_order.price >= best_price) :
        (aggressive_order.price <= best_price);
      if (!can_match) {
        break;
      }

      auto& resting_orders = best_price_it->second;
      if (resting_orders.empty()) {
        opposite_book.erase(best_price_it);
        continue;
      }

      Order& resting_order = resting_orders.front();
      uint64_t trade_quantity = std::min(aggressive_order.quantity, resting_order.quantity);
      double trade_price = resting_order.price;

      emitTradeEvent(trade_quantity, trade_price);

      aggressive_order.quantity -= trade_quantity;
      if (aggressive_order.quantity > 0) {
        emitPartiallyFilled(aggressive_order.order_id, aggressive_order.quantity);
      }
      else {
        emitFullyFilled(aggressive_order.order_id);
      }

      resting_order.quantity -= trade_quantity;
      if (resting_order.quantity == 0) {
        emitFullyFilled(resting_order.order_id);
        m_orderMap.erase(resting_order.order_id);
        resting_orders.pop_front();
        if (resting_orders.empty()) {
          opposite_book.erase(best_price_it);
        }
      }
      else {
        emitPartiallyFilled(resting_order.order_id, resting_order.quantity);
        m_orderMap[resting_order.order_id].quantity = resting_order.quantity;
      }
    }
  }
  else
  {
    auto& opposite_book = m_buyOrderBook;
    while (aggressive_order.quantity > 0 && !opposite_book.empty()) {
      auto best_price_it = opposite_book.begin();
      double best_price = best_price_it->first;
      bool can_match = (aggressive_order.side == Side::Buy) ?
        (aggressive_order.price >= best_price) :
        (aggressive_order.price <= best_price);
      if (!can_match) {
        break;
      }

      auto& resting_orders = best_price_it->second;
      if (resting_orders.empty()) {
        opposite_book.erase(best_price_it);
        continue;
      }

      Order& resting_order = resting_orders.front();
      uint64_t trade_quantity = std::min(aggressive_order.quantity, resting_order.quantity);
      double trade_price = resting_order.price;

      emitTradeEvent(trade_quantity, trade_price);

      aggressive_order.quantity -= trade_quantity;
      if (aggressive_order.quantity > 0) {
        emitPartiallyFilled(aggressive_order.order_id, aggressive_order.quantity);
      }
      else {
        emitFullyFilled(aggressive_order.order_id);
      }

      resting_order.quantity -= trade_quantity;
      if (resting_order.quantity == 0) {
        emitFullyFilled(resting_order.order_id);
        m_orderMap.erase(resting_order.order_id);
        resting_orders.pop_front();
        if (resting_orders.empty()) {
          opposite_book.erase(best_price_it);
        }
      }
      else {
        emitPartiallyFilled(resting_order.order_id, resting_order.quantity);
        m_orderMap[resting_order.order_id].quantity = resting_order.quantity;
      }
    }
  }
}

void MatchingEngine::addToBook(const Order& order) {
  if (order.side == Side::Buy)
  {
    auto& book = m_buyOrderBook;
    book[order.price].push_back(order);
    m_orderMap[order.order_id] = order;
  }
  else
  {
    auto& book = m_sellOrderBook;
    book[order.price].push_back(order);
    m_orderMap[order.order_id] = order;
  }

}

void MatchingEngine::emitTradeEvent(uint64_t quantity, double price) {
  std::stringstream ss;
  ss << "2," << quantity << "," << price;
  logger.log_out(ss.str());
}

void MatchingEngine::emitFullyFilled(uint64_t order_id) {
  std::stringstream ss;
  ss << "3," << order_id;
  logger.log_out(ss.str());
}

void MatchingEngine::emitPartiallyFilled(uint64_t order_id, uint64_t quantity) {
  std::stringstream ss;
  ss << "4," << order_id << "," << quantity;
  logger.log_out(ss.str());
}