#pragma once
#include <array>
#include <cstddef>

struct OrderBookState {
    static constexpr size_t LEVELS = 400;
    static constexpr size_t VALUES_PER_LEVEL = 3;  // price, volume, orders
    static constexpr size_t NUM_DEPTHS = 5;        // 10, 20, 50, 100, 400
    static constexpr size_t NUM_FEATURES = 4;      // volume imbalance, order imbalance, bid/ask VWAP

    // Main orderbook data
    std::array<double, LEVELS * VALUES_PER_LEVEL> bids;  // 1200 values
    std::array<double, LEVELS * VALUES_PER_LEVEL> asks;  // 1200 values
    
    // Market features
    double mid_price_change;
    double mid_price;  // Actual mid-price value (not used as a feature)
    std::array<double, NUM_DEPTHS * NUM_FEATURES> features;  // 20 values

    // State identification
    uint16_t state_id;  // ID from orderbook service (0-65535)

    // Helper methods for accessing data
    inline double* bid_level(size_t level) {
        return &bids[level * VALUES_PER_LEVEL];
    }
    
    inline double* ask_level(size_t level) {
        return &asks[level * VALUES_PER_LEVEL];
    }
    
    inline double& feature_at(size_t depth, size_t feature) {
        return features[depth * NUM_FEATURES + feature];
    }
    
    // Total number of features for tensor creation (mid_price is not included)
    static constexpr size_t TOTAL_FEATURES = 
        LEVELS * VALUES_PER_LEVEL * 2 +  // bids and asks
        1 +                              // mid price change
        NUM_DEPTHS * NUM_FEATURES;       // market features
}; 