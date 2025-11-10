#pragma once
#include <string>
#include <vector>
#include <deque>
#include <stdexcept>
#include <algorithm>
#include <simdjson.h>
#include "websocket_client.hpp"
#include <chrono>
#include "rabbitmq_handler.hpp"
#include <array>
#include <iomanip>

struct OrderBookLevel {
    double price;
    double volume;
    double orders;

    OrderBookLevel() : price(0.0), volume(0.0), orders(0.0) {}
    OrderBookLevel(double p, double v, double o) : price(p), volume(v), orders(o) {}
};

struct PreprocessedLevel {
    double price_change;     // % change from previous state
    double volume_change;    // % change from 10-state average
    double orders_change;    // % change from 10-state average
};

struct OrderBookFeatures {
    static constexpr size_t NUM_DEPTHS = 5;
    static constexpr size_t NUM_FEATURES = 4;
    
    double midPrice;
    std::array<double, 5> volumeImbalance;  // For levels 10, 20, 50, 100, 400
    std::array<double, 5> orderImbalance;   // For levels 10, 20, 50, 100, 400
    std::array<double, 5> bidVwapChange;    // Bid VWAP % change relative to mid price
    std::array<double, 5> askVwapChange;    // Ask VWAP % change relative to mid price
    std::array<size_t, 5> depthLevels = {10, 20, 50, 100, 400};
};

class OrderBookException : public std::runtime_error {
public:
    explicit OrderBookException(const std::string& message) : std::runtime_error(message) {}
};

class OrderBookHandler {
public:
    // Constants for message sizes and binary format
    static constexpr size_t LEVEL_VALUES = 3;  // price, volume, orders
    static constexpr size_t VALUE_SIZE = sizeof(uint64_t);
    static constexpr size_t STATE_ID_SIZE = sizeof(uint16_t);

    // Buffer size constants
    static constexpr size_t HISTORY_SIZE = 10;  // Store last 10 states
    static constexpr uint16_t MAX_STATE_ID = 65535;  // Maximum state ID value (2^16 - 1)
    static constexpr size_t TIMING_BUFFER_SIZE = 100;  // Number of timings to average

    OrderBookHandler(WebSocketClient* client, RabbitMQHandler* rmq) 
        : ws_client_(client), rmq_handler_(rmq), current_state_id_(0), parser_(), json_buffer_() {}
    
    void handleMessage(const std::string& message);
    void subscribe(const std::string& instrument);

private:
    WebSocketClient* ws_client_;
    RabbitMQHandler* rmq_handler_;
    std::vector<OrderBookLevel> bids;
    std::vector<OrderBookLevel> asks;
    uint16_t current_state_id_;  // Current state ID (0-65535)
    
    // JSON parsing
    simdjson::ondemand::parser parser_;
    simdjson::padded_string json_buffer_;

    // Timing tracking
    std::deque<std::chrono::microseconds> processing_times_;
    size_t total_messages_processed_ = 0;
    
    // Historical data for calculating changes
    std::deque<std::vector<OrderBookLevel>> previous_bids;
    std::deque<std::vector<OrderBookLevel>> previous_asks;
    double previous_mid_price = 0.0;

    // Fast string to double conversion
    inline double fast_stod(std::string_view str);

    void handleSnapshot(simdjson::ondemand::value&& data);
    void processOrderBookUpdate(simdjson::ondemand::value&& data);
    void updatePriceLevel(std::vector<OrderBookLevel>& side, simdjson::ondemand::array&& level, bool is_bids);
    void validateOrderBookState();
    void publishOrderBookUpdate();
    void updateHistory();
    void incrementStateId() { current_state_id_ = (current_state_id_ + 1) % (MAX_STATE_ID + 1); }
    void logAverageProcessingTime(std::chrono::microseconds current_duration);

    // Feature calculation methods
    OrderBookFeatures calculateFeatures() const;
    double calculateMidPrice() const;
    double calculateVolumeImbalance(size_t depth) const;
    double calculateOrderImbalance(size_t depth) const;
    double calculateVWAP(size_t depth, bool is_bids) const;
    std::string getCurrentTimestamp() const;

    // Preprocessing methods
    std::vector<PreprocessedLevel> preprocessLevels(
        const std::vector<OrderBookLevel>& current,
        const std::vector<OrderBookLevel>& previous,
        const std::deque<std::vector<OrderBookLevel>>& history
    ) const;

    double calculateAverageVolume(const std::deque<std::vector<OrderBookLevel>>& history, size_t level_idx) const;
    double calculateAverageOrders(const std::deque<std::vector<OrderBookLevel>>& history, size_t level_idx) const;

    static bool compareAsks(const OrderBookLevel& a, const OrderBookLevel& b) {
        return a.price < b.price;  // Ascending order for asks
    }

    static bool compareBids(const OrderBookLevel& a, const OrderBookLevel& b) {
        return a.price > b.price;  // Descending order for bids
    }
};
