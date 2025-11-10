#pragma once
#include <string>
#include <amqp.h>
#include <amqp_tcp_socket.h>
#include <chrono>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <memory>
#include <vector>
#include <unordered_set>
#include <unordered_map>
#include "okx_websocket.hpp"
#include "possizehandler.hpp"

struct Trade {
    bool has_active_trade{false};
    bool is_long{false};
    double size{0.0};
    double maxdd;            // Maximum drawdown for the trade
    double cumulative_reward; // Cumulative reward from partial trade closures
    double total_size{0.0};
    std::string tradeId;
    std::vector<OrderInfo> orders;
    
    // New fields for price tracking
    double buy_side_cumulative_price{0.0};   // Sum of (price * size) for buy orders
    double buy_side_total_size{0.0};         // Total filled size of buy orders
    double sell_side_cumulative_price{0.0};  // Sum of (price * size) for sell orders
    double sell_side_total_size{0.0};        // Total filled size of sell orders
    
    // Helper functions to get average prices
    double get_avg_buy_price() const {
        return buy_side_total_size > 0 ? buy_side_cumulative_price / buy_side_total_size : 0.0;
    }
    
    double get_avg_sell_price() const {
        return sell_side_total_size > 0 ? sell_side_cumulative_price / sell_side_total_size : 0.0;
    }
};

class OMSHandler {
public:
    OMSHandler(const std::string& host, int port,
              const std::string& username, const std::string& password,
              const std::string& okx_api_key,
              const std::string& okx_secret_key,
              const std::string& okx_passphrase);
    ~OMSHandler();

    void start();
    void stop();
    double get_balance() const { return okx_ws_ ? okx_ws_->get_balance() : 0.0; }
    bool is_balance_received() const { return okx_ws_ ? okx_ws_->is_balance_received() : false; }

private:
    // RabbitMQ connection details
    std::string host_;
    int port_;
    std::string username_;
    std::string password_;
    bool is_running_;

    // RabbitMQ connection and channel
    amqp_connection_state_t conn_;
    amqp_socket_t* socket_;

    // OKX WebSocket client
    std::unique_ptr<OKXWebSocket> okx_ws_;

    // Current trade state
    Trade current_trade_;

    // Track published state IDs
    std::unordered_set<uint32_t> published_state_ids_;

    // Maps OKX order ID to state ID for tracking all known orders
    std::unordered_map<std::string, uint32_t> known_orders_;

    // Next trade state
    Trade next_trade_;
    bool has_next_trade_{false};

    // Private methods
    void initializeRabbitMQ();
    void cleanupRabbitMQ();
    void handleMessage(const std::string& message);
    void declareExchangesAndQueues();
    bool initializeOKXWebSocket();
    void publishTradeUpdate(uint32_t state_id, const std::string& okx_id);
    void publishTradeUpdate(uint32_t state_id, 
                          const std::string& okx_id,
                          bool is_trade_closed,
                          const std::vector<std::pair<std::string, double>>& filled_portions);
    void publishTradeUpdate(uint32_t state_id, 
                          const std::string& okx_id,
                          double execution_percentage);
    bool place_order(uint32_t state_id, const std::string& inst_id,
                    const std::string& td_mode, const std::string& side,
                    const std::string& ord_type, double size, double price,
                    double original_volume, double original_price);
    void printTradeOrders() const;
    std::string getCurrentTimestamp() const;
    void processAction(uint8_t action_type, double price, double volume, double mid_price, uint32_t state_id);

    std::unique_ptr<PosSizeHandler> pos_size_handler_;
}; 