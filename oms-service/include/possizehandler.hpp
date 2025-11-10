#pragma once

#include <string>
#include <vector>
#include <cmath>
#include <stdexcept>
#include "okx_websocket.hpp"

struct Trade;

struct PositionState {
    double buy_side_exposure;     // Total exposure on buy side
    double sell_side_exposure;    // Total exposure on sell side
    std::string current_trade_side;  // "buy", "sell", or "none"
    double current_trade_size;    // Size of current trade (0 if none)
};

struct PosSizeResult {
    bool can_place_order;
    double adjusted_size;
    bool was_adjusted;
    std::string reason;
    std::string calculation_log;
    PositionState position_state;  // Current state of all positions
    double available_side_space;   // Available space for the requested side
    double max_allowed_contracts;  // Maximum allowed contracts per side
};

class PosSizeHandler {
public:
    explicit PosSizeHandler(double margin_percentage) 
        : margin_percentage_(margin_percentage), 
          leverage_(100.0) {
        if (margin_percentage <= 0 || margin_percentage > 100) {
            throw std::invalid_argument("Margin percentage must be between 0 and 100");
        }
    }

    PosSizeResult validateAndAdjustSize(
        double requested_size,
        const std::string& side,
        double total_capital,
        const Trade& current_trade,
        const std::vector<OrderInfo>& pending_orders,
        double mid_price) const;

private:
    double margin_percentage_;
    const double leverage_;
    static constexpr double MIN_CONTRACT_SIZE = 0.1;  // Minimum contract size for OKX

    double calculateMaxAllowedContracts(double total_capital, double mid_price) const;
    
    PositionState getCurrentPositionState(
        const Trade& current_trade,
        const std::vector<OrderInfo>& pending_orders
    ) const;

    // Helper method to validate side string
    bool isValidSide(const std::string& side) const {
        return side == "buy" || side == "sell";
    }
};