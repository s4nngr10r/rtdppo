#pragma once

#include <string>
#include <functional>
#include <memory>
#include <libwebsockets.h>
#include <nlohmann/json.hpp>
#include <atomic>
#include <mutex>
#include <thread>
#include <deque>
#include <vector>
#include <iomanip>
#include <chrono>
#include <sstream>
#include <queue>

// Structure to store order information
struct OrderInfo {
    uint32_t state_id;  // Internal state ID from input
    double volume;      // Original order volume
    double price;       // Original order price
    std::string okx_order_id;  // OKX order ID
    bool has_okx_id{false};    // Flag indicating if OKX ID is set
    double filled_size{0.0};   // Actual filled size
    double cumulative_filled_size{0.0};  // Track total filled size across all fills
    double avg_fill_price{0.0}; // Average fill price
    bool is_filled{false};     // Whether the order is filled
    double execution_percentage;  // Track percentage of order executed
    std::string order_state;     // Track order state (filled, partially_filled, etc.)
    std::string side;            // Order side (buy or sell)
    std::string tradeId;         // ID of the trade this order belongs to
    int64_t fill_time{0};        // Timestamp when order was filled (from OKX fillTime)
    
    // New fields for tracking fill portions
    struct FillPortion {
        std::string tradeId;     // ID of the trade this portion belongs to
        double size;             // Size of this fill portion
        double price;            // Price of this fill portion
        int64_t timestamp;       // When this portion was filled
        bool is_closing{false};  // Whether this portion was used to close a position
        double execution_percentage;  // Added execution percentage field
    };
    std::vector<FillPortion> fill_portions;  // Track all fill portions and their associated trades
};

// Structure to track cancellation requests
struct CancellationInfo {
    std::string okx_order_id;
    bool cancellation_sent{false};
    bool cancellation_confirmed{false};
};

// Structure to store buffered order updates
struct BufferedOrderUpdate {
    std::string okx_order_id;
    double filled_size;
    double avg_price;
    std::string side;
    std::string state;
    double pnl;
    int64_t timestamp;  // fillTime or uTime
    nlohmann::json raw_data;
    double fill_delta{0.0};  // Track the actual fill delta for this update
};

class OKXWebSocket {
public:
    static OKXWebSocket* instance_;
    
    OKXWebSocket(const std::string& api_key, 
                 const std::string& secret_key, 
                 const std::string& passphrase);
    ~OKXWebSocket();

    bool connect();
    void disconnect();
    bool fetch_balance();
    double get_balance() const { return initial_balance_.load(); }
    bool is_balance_received() const { return balance_received_.load(); }

    // Max drawdown getter and setter
    double get_maxdd() const { return maxdd_.load(); }
    void update_maxdd(double new_maxdd) { maxdd_.store(new_maxdd); }

    // Method for sending orders
    bool send_order(uint32_t state_id,
                   const std::string& inst_id,
                   const std::string& td_mode,
                   const std::string& side,
                   const std::string& ord_type,
                   double size,
                   double price,
                   double original_volume,
                   double original_price);

    // Methods for subscribing to channels
    bool subscribe_to_orders();
    bool subscribe_to_positions();

    // Callback types for order updates
    using OrderIdCallback = std::function<void(uint32_t state_id, const std::string& okx_order_id)>;
    using OrderFillCallback = std::function<void(const std::string& okx_order_id, 
                                               double filled_size,
                                               double avg_price,
                                               const std::string& side,
                                               const std::string& state,
                                               double pnl,
                                               int64_t fill_time)>;

    void set_order_id_callback(OrderIdCallback callback) {
        order_id_callback_ = callback;
    }

    void set_order_fill_callback(OrderFillCallback callback) {
        order_fill_callback_ = callback;
    }

    // Methods for order management
    void update_order_id(uint32_t state_id, const std::string& okx_order_id, bool has_id);
    void update_order_fill(const std::string& okx_order_id, 
                          double filled_size, 
                          double avg_price,
                          const std::string& side,
                          const std::string& state);
    void store_order(const OrderInfo& order);
    void process_old_orders();
    bool send_cancel_order(const std::string& okx_order_id);
    void log_orders() const;
    
    // New methods for order cleanup
    void remove_filled_order(const std::string& okx_order_id);
    void move_to_old_orders(const OrderInfo& order);
    void cleanup_orders();

    // Deque access for testing/debugging
    std::deque<OrderInfo> orders_;
    std::mutex orders_mutex_;

    // Helper method for timestamps
    std::string getCurrentTimestamp() const {
        auto now = std::chrono::system_clock::now();
        auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()
        );
        auto now_sec = std::chrono::duration_cast<std::chrono::seconds>(now_ms);
        auto ms = now_ms.count() % 1000;

        std::time_t now_time = std::chrono::system_clock::to_time_t(now);
        std::stringstream ss;
        ss << std::put_time(std::localtime(&now_time), "%Y-%m-%d %H:%M:%S");
        ss << '.' << std::setfill('0') << std::setw(3) << ms;
        return ss.str();
    }

private:
    static int callback_function(struct lws* wsi, 
                               enum lws_callback_reasons reason,
                               void* user, 
                               void* in, 
                               size_t len);

    void handle_message(const std::string& message);
    void handle_order_update(const std::string& message);
    void handle_position_update(const nlohmann::json& data);
    void handle_cancel_response(const nlohmann::json& data);
    std::string generate_auth_message() const;
    std::string sign_message(const std::string& timestamp, 
                           const std::string& method,
                           const std::string& request_path,
                           const std::string& body = "") const;
    bool send_ws_message(const std::string& message);

    struct lws_context* context_;
    struct lws* connection_;
    std::string api_key_;
    std::string secret_key_;
    std::string passphrase_;
    std::atomic<double> initial_balance_{0.0};
    std::atomic<bool> balance_received_{false};
    std::atomic<bool> connected_{false};
    std::mutex mutex_;
    std::thread service_thread_;

    // Storage for old orders that need to be cancelled
    std::vector<CancellationInfo> old_orders_;
    std::mutex old_orders_mutex_;  // Separate mutex for old orders

    // WebSocket connection details
    static constexpr const char* WSS_HOST = "wspap.okx.com";
    static constexpr const char* WSS_PATH = "/ws/v5/private";
    static constexpr int WSS_PORT = 8443;
    static constexpr const char* WSS_PROTOCOL = "ws";
    static constexpr size_t RX_BUFFER_SIZE = 65536;
    static constexpr int MAX_RETRIES = 50;

    // Callbacks for order updates
    OrderIdCallback order_id_callback_;
    OrderFillCallback order_fill_callback_;

    std::atomic<double> maxdd_{0.0};  // Add maxdd atomic variable

    // Message queue for sending WebSocket messages
    std::queue<std::string> send_queue_;
    std::mutex send_queue_mutex_;

    // Buffer for order updates
    static constexpr int64_t BUFFER_WINDOW_MS = 2000;  // 2 second buffer window
    std::vector<BufferedOrderUpdate> update_buffer_;
    std::mutex buffer_mutex_;
    std::thread buffer_processor_thread_;
    std::atomic<bool> buffer_processor_running_{false};

    // Buffer processing methods
    void start_buffer_processor();
    void stop_buffer_processor();
    void buffer_processor_loop();
    void process_buffered_updates();
    void add_to_buffer(const BufferedOrderUpdate& update);
    int64_t get_current_timestamp_ms() const;
}; 