#include "../include/okx_websocket.hpp"
#include <iostream>
#include <openssl/hmac.h>
#include <openssl/sha.h>
#include <openssl/evp.h>
#include <openssl/bio.h>
#include <openssl/buffer.h>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <thread>
#include <binary_utils.hpp>
#include <stdexcept>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

OKXWebSocket* OKXWebSocket::instance_ = nullptr;

// Helper function for base64 encoding using OpenSSL
std::string base64_encode(const unsigned char* input, int length) {
    BIO *bio, *b64;
    BUF_MEM *bufferPtr;

    b64 = BIO_new(BIO_f_base64());
    bio = BIO_new(BIO_s_mem());
    bio = BIO_push(b64, bio);

    BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
    BIO_write(bio, input, length);
    BIO_flush(bio);
    BIO_get_mem_ptr(bio, &bufferPtr);

    std::string result(bufferPtr->data, bufferPtr->length);

    BIO_free_all(bio);

    return result;
}

OKXWebSocket::OKXWebSocket(const std::string& api_key, 
                         const std::string& secret_key, 
                         const std::string& passphrase)
    : api_key_(api_key)
    , secret_key_(secret_key)
    , passphrase_(passphrase)
    , context_(nullptr)
    , connection_(nullptr)
    , initial_balance_(0.0)
    , balance_received_(false)
    , connected_(false)
    , send_queue_() {
    instance_ = this;
    start_buffer_processor();
}

OKXWebSocket::~OKXWebSocket() {
    stop_buffer_processor();
    disconnect();
}

void OKXWebSocket::start_buffer_processor() {
    buffer_processor_running_ = true;
    buffer_processor_thread_ = std::thread(&OKXWebSocket::buffer_processor_loop, this);
}

void OKXWebSocket::stop_buffer_processor() {
    buffer_processor_running_ = false;
    if (buffer_processor_thread_.joinable()) {
        buffer_processor_thread_.join();
    }
}

void OKXWebSocket::buffer_processor_loop() {
    while (buffer_processor_running_) {
        process_buffered_updates();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));  // Check every 100ms
    }
}

int64_t OKXWebSocket::get_current_timestamp_ms() const {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

void OKXWebSocket::add_to_buffer(const BufferedOrderUpdate& update) {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    update_buffer_.push_back(update);
    
    // Sort buffer by timestamp
    std::sort(update_buffer_.begin(), update_buffer_.end(),
              [](const BufferedOrderUpdate& a, const BufferedOrderUpdate& b) {
                  return a.timestamp < b.timestamp;
              });
}

void OKXWebSocket::process_buffered_updates() {
    std::vector<BufferedOrderUpdate> updates_to_process;
    
    {
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        if (update_buffer_.empty()) {
            return;
        }

        int64_t current_time = get_current_timestamp_ms();
        
        // Process updates older than buffer window
        auto split_point = std::partition(update_buffer_.begin(), update_buffer_.end(),
            [current_time, this](const BufferedOrderUpdate& update) {
                return (current_time - update.timestamp) > BUFFER_WINDOW_MS;
            });
        
        updates_to_process.insert(
            updates_to_process.end(),
            std::make_move_iterator(update_buffer_.begin()),
            std::make_move_iterator(split_point)
        );
        
        update_buffer_.erase(update_buffer_.begin(), split_point);
    }
    
    // Sort updates by timestamp
    std::sort(updates_to_process.begin(), updates_to_process.end(),
              [](const BufferedOrderUpdate& a, const BufferedOrderUpdate& b) {
                  return a.timestamp < b.timestamp;
              });
    
    // Process updates in timestamp order
    for (const auto& update : updates_to_process) {
        if (order_fill_callback_) {
            order_fill_callback_(
                update.okx_order_id,
                update.filled_size,
                update.avg_price,
                update.side,
                update.state,
                update.pnl,
                update.timestamp
            );
        }
    }
}

void OKXWebSocket::disconnect() {
    connected_ = false;
    balance_received_ = false;
    
    if (service_thread_.joinable()) {
        service_thread_.join();
    }
    
    if (connection_) {
        lws_callback_on_writable(connection_);
        connection_ = nullptr;
    }
    if (context_) {
        lws_context_destroy(context_);
        context_ = nullptr;
    }
}

std::string OKXWebSocket::sign_message(const std::string& timestamp,
                                    const std::string& method,
                                    const std::string& request_path,
                                    const std::string& body) const {
    std::string pre_hash = timestamp + method + request_path + body;
    unsigned char* digest = HMAC(EVP_sha256(),
                               secret_key_.c_str(), secret_key_.length(),
                               reinterpret_cast<const unsigned char*>(pre_hash.c_str()),
                               pre_hash.length(), nullptr, nullptr);
    return base64_encode(digest, 32);
}

std::string OKXWebSocket::generate_auth_message() const {
    auto now = std::chrono::system_clock::now();
    auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count() / 1000.0;
    
    std::stringstream ss;
    ss << std::fixed << std::setprecision(3) << timestamp;
    std::string timestamp_str = ss.str();
    
    std::string sign = sign_message(timestamp_str, "GET", "/users/self/verify", "");
    
    nlohmann::json auth_msg = {
        {"op", "login"},
        {"args", {{
            {"apiKey", api_key_},
            {"passphrase", passphrase_},
            {"timestamp", timestamp_str},
            {"sign", sign}
        }}}
    };
    
    return auth_msg.dump();
}

bool OKXWebSocket::fetch_balance() {
    if (!connected_ || !connection_) {
        return false;
    }

    nlohmann::json balance_req = {
        {"op", "subscribe"},
        {"args", {{
            {"channel", "account"},
            {"ccy", "USDT"}
        }}}
    };

    std::string message = balance_req.dump();
    return send_ws_message(message);
}

void OKXWebSocket::handle_message(const std::string& message) {
    try {
        auto j = nlohmann::json::parse(message);

        // Handle authentication response
        if (j.contains("event") && j["event"] == "login") {
            if (j.contains("code") && j["code"] == "0") {
                fetch_balance();
                subscribe_to_orders();
                subscribe_to_positions();
            } else {
                std::cerr << "\033[1;31mAuthentication failed: " << message << "\033[0m" << std::endl;
                connection_ = nullptr;  // Reset connection on auth failure
            }
            return;
        }

        // Handle order creation response
        if (j.contains("op") && j["op"] == "order" && j.contains("data")) {
            const auto& data = j["data"];
            if (data.is_array() && !data.empty()) {
                const auto& order = data[0];
                std::string client_order_id = order.contains("clOrdId") ? order["clOrdId"].get<std::string>() : "";
                
                // Check for error in order placement
                if (j.contains("code") && j["code"] != "0" || 
                    order.contains("sCode") && order["sCode"] != "0") {
                    
                    std::string error_msg = order.contains("sMsg") ? order["sMsg"].get<std::string>() : 
                                          (j.contains("msg") ? j["msg"].get<std::string>() : "Unknown error");
                    
                    // Remove the failed order from deque
                    if (!client_order_id.empty()) {
                        try {
                            uint32_t state_id = std::stoul(client_order_id);
                            std::lock_guard<std::mutex> lock(orders_mutex_);
                            orders_.erase(
                                std::remove_if(
                                    orders_.begin(),
                                    orders_.end(),
                                    [state_id](const OrderInfo& order) {
                                        return order.state_id == state_id && !order.has_okx_id;
                                    }
                                ),
                                orders_.end()
                            );
                        } catch (const std::exception& e) {
                            std::cerr << "\033[1;31mError processing failed order state ID: " << e.what() << "\033[0m" << std::endl;
                        }
                    }
                    
                    std::cerr << "\033[1;31m[" << getCurrentTimestamp() << "] Order placement failed for ID " 
                              << client_order_id << ": " << error_msg << "\033[0m" << std::endl;
                    return;
                }

                // Process successful order
                if (order.contains("ordId") && order.contains("clOrdId")) {
                    std::string okx_order_id = order["ordId"].get<std::string>();
                    try {
                        uint32_t state_id = std::stoul(client_order_id);
                        if (order_id_callback_) {
                            order_id_callback_(state_id, okx_order_id);
                        }
                    } catch (const std::exception& e) {
                        std::cerr << "\033[1;31mError processing order ID: " << e.what() << "\033[0m" << std::endl;
                    }
                }
            }
            return;
        }

        // Handle balance update
        if (j.contains("arg") && j["arg"].contains("channel") && j["arg"]["channel"] == "account") {
            if (j.contains("data") && j["data"].is_array() && !j["data"].empty()) {
                const auto& account_data = j["data"][0];
                if (account_data.contains("details") && account_data["details"].is_array() && !account_data["details"].empty()) {
                    const auto& details = account_data["details"][0];
                    if (details.contains("ccy") && details["ccy"] == "USDT" && details.contains("cashBal")) {
                        double new_balance = std::stod(details["cashBal"].get<std::string>());
                        initial_balance_.store(new_balance);
                        balance_received_ = true;
                    }
                }
            }
            return;
        }

        // Handle other channel messages
        if (j.contains("arg") && j["arg"].contains("channel")) {
            std::string channel = j["arg"]["channel"];
            if (channel == "orders") {
                handle_order_update(message);
            } else if (channel == "positions" && j.contains("data")) {
                handle_position_update(j["data"]);
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "Error parsing message: " << e.what() << "\nMessage: " << message << std::endl;
    }
}

int OKXWebSocket::callback_function(struct lws* wsi,
                                 enum lws_callback_reasons reason,
                                 void* user,
                                 void* in,
                                 size_t len) {
    if (!instance_) return 0;

    switch (reason) {
        case LWS_CALLBACK_CLIENT_ESTABLISHED: {
            std::cout << "WebSocket connection established, authenticating..." << std::endl;
            instance_->connected_ = true;
            
            // Send authentication message immediately after connection
            std::string auth_message = instance_->generate_auth_message();
            if (!instance_->send_ws_message(auth_message)) {
                std::cerr << "Failed to send authentication message" << std::endl;
                return -1;
            }
            break;
        }
        case LWS_CALLBACK_CLIENT_RECEIVE: {
            if (len > 0) {
                // Log raw message
                std::cout << "[" << instance_->getCurrentTimestamp() << "] Raw WS Message Received (" << len << " bytes): "
                          << std::string(static_cast<char*>(in), len) << std::endl;
                
                // Process message
                instance_->handle_message(std::string(static_cast<char*>(in), len));
            }
            break;
        }
        case LWS_CALLBACK_CLIENT_CONNECTION_ERROR: {
            std::string error = in ? std::string(static_cast<char*>(in), len) : "Unknown error";
            std::cerr << "WebSocket connection error: " << error << std::endl;
            instance_->connection_ = nullptr;
            break;
        }
        case LWS_CALLBACK_CLIENT_CLOSED: {
            std::cerr << "WebSocket connection closed, will retry..." << std::endl;
            instance_->connection_ = nullptr;
            instance_->balance_received_ = false;
            break;
        }
        case LWS_CALLBACK_WSI_DESTROY: {
            instance_->connection_ = nullptr;
            instance_->balance_received_ = false;
            break;
        }
        case LWS_CALLBACK_CLIENT_WRITEABLE: {
            std::lock_guard<std::mutex> lock(instance_->send_queue_mutex_);
            if (!instance_->send_queue_.empty()) {
                std::string msg = instance_->send_queue_.front();
                instance_->send_queue_.pop();
                
                // Log raw outgoing message
                std::cout << "[" << instance_->getCurrentTimestamp() << "] Raw WS Message Sending: " << msg << std::endl;
                
                // Prepare and send the message
                std::vector<unsigned char> buf(LWS_PRE + msg.length());
                memcpy(buf.data() + LWS_PRE, msg.c_str(), msg.length());
                lws_write(wsi, buf.data() + LWS_PRE, msg.length(), LWS_WRITE_TEXT);
            }
            break;
        }
        default:
            break;
    }
    return 0;
}

bool OKXWebSocket::connect() {
    struct lws_context_creation_info info;
    memset(&info, 0, sizeof(info));
    
    info.port = CONTEXT_PORT_NO_LISTEN;
    info.protocols = new lws_protocols[2]{
        {
            WSS_PROTOCOL,
            callback_function,
            0,
            RX_BUFFER_SIZE,
            0,
            nullptr,
            0
        },
        { nullptr, nullptr, 0, 0, 0, nullptr, 0 }
    };
    
    info.gid = -1;
    info.uid = -1;
    info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
    info.fd_limit_per_thread = 1 + 1 + 1;
    info.retry_and_idle_policy = nullptr;
    info.client_ssl_private_key_password = nullptr;
    info.client_ssl_cert_filepath = nullptr;
    info.client_ssl_private_key_filepath = nullptr;
    info.client_ssl_ca_filepath = nullptr;
    
    context_ = lws_create_context(&info);
    if (!context_) {
        std::cerr << "Failed to create WebSocket context" << std::endl;
        return false;
    }

    // Initialize connection info
    struct lws_client_connect_info ccinfo;
    memset(&ccinfo, 0, sizeof(ccinfo));
    ccinfo.context = context_;
    ccinfo.port = WSS_PORT;
    ccinfo.address = WSS_HOST;
    ccinfo.path = WSS_PATH;
    ccinfo.host = WSS_HOST;
    ccinfo.origin = WSS_HOST;
    ccinfo.protocol = WSS_PROTOCOL;
    ccinfo.ssl_connection = LCCSCF_USE_SSL | LCCSCF_ALLOW_SELFSIGNED | 
                           LCCSCF_SKIP_SERVER_CERT_HOSTNAME_CHECK |
                           LCCSCF_ALLOW_EXPIRED;
    ccinfo.userdata = this;
    
    // Start WebSocket service thread
    connected_ = true;
    service_thread_ = std::thread([this, ccinfo]() mutable {
        int retry_count = 0;
        auto start_time = std::chrono::steady_clock::now();
        
        while (connected_ && retry_count < MAX_RETRIES) {
            if (!connection_) {
                connection_ = lws_client_connect_via_info(&ccinfo);
                if (!connection_) {
                    retry_count++;
                    std::this_thread::sleep_for(std::chrono::seconds(2));
                    continue;
                }
            }
            
            // Process events continuously
            lws_service(context_, 0);
            
            // Check if we've successfully received the balance
            if (balance_received_) {
                if (retry_count > 0) {  // Only print on initial connection or after retry
                    retry_count = 0;  // Reset retry count on success
                }
                start_time = std::chrono::steady_clock::now();
            } else {
                // Check if we've been trying too long without success
                auto current_time = std::chrono::steady_clock::now();
                auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                    current_time - start_time).count();
                
                if (elapsed > 5) {  // Reduced timeout to 5 seconds
                    if (connection_) {
                        std::string auth_message = generate_auth_message();
                        send_ws_message(auth_message);
                    }
                    start_time = current_time;
                }
            }
            
            // Small sleep to prevent CPU spinning
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        
        if (retry_count >= MAX_RETRIES) {
            std::cerr << "Max retry attempts reached" << std::endl;
            connected_ = false;
        }
    });
    
    // Wait for initial connection and authentication
    int wait_seconds = 0;
    while (!balance_received_ && wait_seconds < 30 && connected_) {  // Wait up to 30 seconds
        std::this_thread::sleep_for(std::chrono::seconds(1));
        wait_seconds++;
    }
    
    if (!balance_received_) {
        std::cerr << "Failed to establish connection with OKX after " << wait_seconds << " seconds" << std::endl;
        disconnect();
        return false;
    }
    
    return true;
}

bool OKXWebSocket::send_order(uint32_t state_id,
                            const std::string& inst_id,
                            const std::string& td_mode,
                            const std::string& side,
                            const std::string& ord_type,
                            double size,
                            double price,
                            double original_volume,
                            double original_price) {
    if (!connected_ || !connection_) {
        std::cerr << "\033[1;31m[ERROR] Cannot send order: WebSocket not connected\033[0m" << std::endl;
        return false;
    }

    try {
        // Store initial order info in deque
        OrderInfo order;
        order.state_id = state_id;
        order.volume = size;  // Store the actual calculated size instead of original_volume
        order.price = price;
        order.has_okx_id = false;
        order.is_filled = false;
        order.filled_size = 0;
        order.avg_fill_price = 0;
        order.side = side;  // Set the side when creating the order
        order.order_state = "pending";  // Set initial state
        
        store_order(order);

        // Create order message with calculated trading values
        nlohmann::json order_args = {
            {"instId", inst_id},
            {"tdMode", td_mode},
            {"side", side},
            {"ordType", ord_type},
            {"sz", std::to_string(size)},
            {"clOrdId", std::to_string(state_id)}  // Add client order ID
        };

        // Add price for limit orders
        if (ord_type == "limit") {
            order_args["px"] = std::to_string(price);
        }

        nlohmann::json order_message = {
            {"id", std::to_string(state_id)},  // Use state_id as the request ID
            {"op", "order"},
            {"args", {order_args}}
        };

        // Send the order message
        std::string message = order_message.dump();
        bool send_result = send_ws_message(message);
        
        return send_result;

    } catch (const std::exception& e) {
        std::cerr << "\033[1;31m[ERROR] Exception while sending order: " << e.what() << "\033[0m" << std::endl;
        return false;
    }
}

bool OKXWebSocket::send_ws_message(const std::string& message) {
    if (!connected_ || !connection_) {
        return false;
    }

    // Log raw message being queued
    std::cout << "[" << getCurrentTimestamp() << "] Raw WS Message Queued: " << message << std::endl;
    
    {
        std::lock_guard<std::mutex> lock(send_queue_mutex_);
        send_queue_.push(message);
    }
    lws_callback_on_writable(connection_);
    return true;
}

bool OKXWebSocket::subscribe_to_orders() {
    if (!connected_ || !connection_) {
        std::cerr << "WebSocket not connected" << std::endl;
        return false;
    }

    nlohmann::json subscribe_msg = {
        {"op", "subscribe"},
        {"args", {
            {
                {"channel", "orders"},
                {"instType", "SWAP"},
                {"instId", "BTC-USDT-SWAP"}
            }
        }}
    };

    std::string message = subscribe_msg.dump();
    return send_ws_message(message);
}

bool OKXWebSocket::subscribe_to_positions() {
    if (!connected_ || !connection_) {
        std::cerr << "WebSocket not connected" << std::endl;
        return false;
    }

    nlohmann::json subscribe_msg = {
        {"op", "subscribe"},
        {"args", {
            {
                {"channel", "positions"},
                {"instType", "SWAP"},
                {"instId", "BTC-USDT-SWAP"}
            }
        }}
    };

    std::string message = subscribe_msg.dump();
    return send_ws_message(message);
}

void OKXWebSocket::handle_order_update(const std::string& message) {
    try {
        auto json = nlohmann::json::parse(message);
        
        for (const auto& data : json["data"]) {
            try {
                std::string okx_order_id = data["ordId"].get<std::string>();
                std::string state = data["state"].get<std::string>();
                double acc_filled_size = std::stod(data["accFillSz"].get<std::string>());  // Use accumulated fill size
                double avg_price = std::stod(data["avgPx"].get<std::string>());
                std::string side = data["side"].get<std::string>();
                double pnl = data.contains("pnl") ? std::stod(data["pnl"].get<std::string>()) : 0.0;
                
                // Get timestamp with robust error handling
                int64_t timestamp = 0;
                try {
                    if (data.contains("fillTime") && !data["fillTime"].get<std::string>().empty()) {
                        timestamp = std::stoll(data["fillTime"].get<std::string>());
                    } else if (data.contains("uTime") && !data["uTime"].get<std::string>().empty()) {
                        timestamp = std::stoll(data["uTime"].get<std::string>());
                    } else if (data.contains("cTime") && !data["cTime"].get<std::string>().empty()) {
                        timestamp = std::stoll(data["cTime"].get<std::string>());
                    } else {
                        timestamp = get_current_timestamp_ms();
                        std::cerr << "Warning: No valid timestamp found in order update, using current time" << std::endl;
                    }
                } catch (const std::exception& e) {
                    timestamp = get_current_timestamp_ms();
                    std::cerr << "Warning: Failed to convert timestamp: " << e.what() << ", using current time" << std::endl;
                }

                // Find previous accumulated fill size
                double prev_acc_filled_size = 0.0;
                {
                    std::lock_guard<std::mutex> lock(orders_mutex_);
                    for (const auto& order : orders_) {
                        if (order.okx_order_id == okx_order_id) {
                            prev_acc_filled_size = order.cumulative_filled_size;  // Track cumulative fills
                            break;
                        }
                    }
                }

                // Calculate actual fill delta
                double fill_delta = acc_filled_size - prev_acc_filled_size;

                // Only process if there's a real fill delta
                if (fill_delta > 1e-8) {  // Use small epsilon for floating point comparison
                    // Create buffered update with the fill delta
                    BufferedOrderUpdate update{
                        okx_order_id,
                        acc_filled_size,  // Store total accumulated fill
                        avg_price,
                        side,
                        state,
                        pnl,
                        timestamp,
                        data
                    };
                    update.fill_delta = fill_delta;  // Store the actual fill delta
                    
                    // Add to buffer
                    add_to_buffer(update);
                }

            } catch (const std::exception& e) {
                std::cerr << "Error processing order: " << e.what() << "\nData: " << data.dump() << std::endl;
                continue;
            }
        }

    } catch (const std::exception& e) {
        std::cerr << "Error handling order update: " << e.what() << "\nMessage: " << message << std::endl;
    }
}

void OKXWebSocket::handle_position_update(const nlohmann::json& data) {
    try {
        for (const auto& position : data) {
            if (position.contains("instId") && position["instId"] == "BTC-USDT-SWAP") {
                // Extract uplRatio and convert to double with robust error handling
                if (position.contains("uplRatio")) {
                    try {
                        std::string uplRatioStr = position["uplRatio"].get<std::string>();
                        // Skip empty strings or invalid values
                        if (uplRatioStr.empty() || uplRatioStr == "null" || uplRatioStr == "-") {
                            continue;
                        }
                        double uplRatio = std::stod(uplRatioStr);
                        
                        // Only update maxdd if we have an active trade and uplRatio is negative
                        if (instance_ && uplRatio < 0) {
                            // Get current maxdd through OMSHandler
                            double current_maxdd = instance_->get_maxdd();
                            
                            // If current uplRatio is more negative than maxdd, update it
                            if (uplRatio < current_maxdd) {
                                instance_->update_maxdd(uplRatio);
                            }
                        }
                    } catch (const std::exception& e) {
                        // Log the error but continue processing
                        std::cerr << "Error converting uplRatio: " << e.what() 
                                  << ", Value: " << position["uplRatio"].dump() << std::endl;
                    }
                }
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "Error handling position update: " << e.what() << std::endl;
    }
}

void OKXWebSocket::store_order(const OrderInfo& order) {
    std::lock_guard<std::mutex> lock(orders_mutex_);
    
    // Ensure volume is properly set
    OrderInfo new_order = order;
    if (new_order.volume <= 0) {
        new_order.volume = std::max(new_order.filled_size, 0.1);  // Use at least 0.1 as minimum volume
    }
    
    // Calculate initial execution percentage
    if (new_order.is_filled || new_order.order_state == "filled") {
        new_order.execution_percentage = 1.0;
    } else {
        new_order.execution_percentage = (new_order.volume > 0) ? (new_order.filled_size / new_order.volume) : 0.0;
    }
    
    orders_.push_back(new_order);

    // If we exceed 300 orders, move oldest orders to cancellation queue
    if (orders_.size() > 300) {
        while (orders_.size() > 300) {
            // Only move to cancellation queue if the order has an OKX ID and isn't already filled
            if (orders_.front().has_okx_id && !orders_.front().is_filled) {
                move_to_old_orders(orders_.front());
            }
            orders_.pop_front();  // Remove oldest order after potentially moving to cancellation
        }
    }
}

void OKXWebSocket::process_old_orders() {
    std::lock_guard<std::mutex> lock(old_orders_mutex_);
    
    // Remove confirmed cancellations
    old_orders_.erase(
        std::remove_if(
            old_orders_.begin(),
            old_orders_.end(),
            [](const CancellationInfo& info) {
                return info.cancellation_confirmed;
            }
        ),
        old_orders_.end()
    );
    
    // Send cancellation for the next order that hasn't been sent
    for (auto& order : old_orders_) {
        if (!order.cancellation_sent) {
            if (send_cancel_order(order.okx_order_id)) {
                order.cancellation_sent = true;
                break;  // Only send one cancellation at a time
            }
        }
    }
}

bool OKXWebSocket::send_cancel_order(const std::string& okx_order_id) {
    if (!connected_ || !connection_) {
        return false;
    }

    try {
        nlohmann::json cancel_message = {
            {"id", std::to_string(std::time(nullptr))},  // Use timestamp as unique ID
            {"op", "cancel-order"},
            {"args", {{
                {"instId", "BTC-USDT-SWAP"},
                {"ordId", okx_order_id}
            }}}
        };

        std::string message = cancel_message.dump();
        return send_ws_message(message);

    } catch (const std::exception& e) {
        std::cerr << "Error creating cancel order message: " << e.what() << std::endl;
        return false;
    }
}

void OKXWebSocket::move_to_old_orders(const OrderInfo& order) {
    std::lock_guard<std::mutex> lock(old_orders_mutex_);
    CancellationInfo cancel_info;
    cancel_info.okx_order_id = order.okx_order_id;
    cancel_info.cancellation_sent = false;
    cancel_info.cancellation_confirmed = false;
    old_orders_.push_back(cancel_info);
}

void OKXWebSocket::cleanup_orders() {
    std::lock_guard<std::mutex> lock(orders_mutex_);
    
    // Only remove filled orders that have been processed (execution update sent)
    auto it = orders_.begin();
    while (it != orders_.end()) {
        if (it->is_filled && it->execution_percentage > 0.0) {  // Only remove if execution % was calculated
            it = orders_.erase(it);
        } else {
            ++it;
        }
    }
    
    // Limit deque size to 300 by removing oldest orders if necessary
    while (orders_.size() > 300) {
        if (orders_.front().has_okx_id && !orders_.front().is_filled) {
            move_to_old_orders(orders_.front());
        }
        orders_.pop_front();
    }
}

void OKXWebSocket::handle_cancel_response(const nlohmann::json& j) {
    try {
        if (j["code"] == "0" && j.contains("data")) {
            const auto& data = j["data"];
            if (data.is_array() && !data.empty()) {
                const auto& cancel_data = data[0];
                if (cancel_data["sCode"] == "0") {
                    std::string okx_order_id = cancel_data["ordId"].get<std::string>();
                    
                    // Mark the cancellation as confirmed
                    std::lock_guard<std::mutex> lock(old_orders_mutex_);
                    for (auto& old_order : old_orders_) {
                        if (old_order.okx_order_id == okx_order_id) {
                            old_order.cancellation_confirmed = true;
                            break;
                        }
                    }
                    
                    // Clean up confirmed cancellations
                    process_old_orders();
                }
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "Error handling cancel response: " << e.what() << std::endl;
    }
}

void OKXWebSocket::update_order_id(uint32_t state_id, const std::string& okx_order_id, bool has_okx_id) {
    std::lock_guard<std::mutex> lock(orders_mutex_);
    for (auto& order : orders_) {
        if (order.state_id == state_id && !order.has_okx_id) {
            order.okx_order_id = okx_order_id;
            order.has_okx_id = has_okx_id;
            order.order_state = "live";  // Set initial state to live when we get OKX ID
            break;
        }
    }
}

void OKXWebSocket::update_order_fill(const std::string& okx_order_id, 
                                   double filled_size, 
                                   double avg_price,
                                   const std::string& side,
                                   const std::string& state) {
    std::lock_guard<std::mutex> lock(orders_mutex_);
    for (auto& order : orders_) {
        if (order.okx_order_id == okx_order_id) {
            // Store previous values for comparison
            double prev_cumulative_filled = order.cumulative_filled_size;
            
            // If order is filled, use the original volume as the filled size
            if (state == "filled") {
                filled_size = order.volume;
            }
            
            // Calculate fill delta (new fills since last update)
            double fill_delta = filled_size - prev_cumulative_filled;
            
            // Only update if we have new fills
            if (fill_delta > 0) {
                // Update order fields atomically
                order.filled_size = fill_delta;  // Store this fill's size
                order.cumulative_filled_size = filled_size;  // Update cumulative filled size
                order.avg_fill_price = avg_price;
                order.order_state = state;
                order.side = side;
                
                // Only add new fill portion if we have a real fill
                if (fill_delta > 0) {
                    OrderInfo::FillPortion new_portion;
                    new_portion.tradeId = order.tradeId;
                    new_portion.size = fill_delta;  // Use the actual fill delta
                    new_portion.price = avg_price;
                    new_portion.timestamp = order.fill_time;
                    
                    // Check if this exact fill portion already exists
                    bool duplicate = false;
                    for (const auto& existing : order.fill_portions) {
                        if (existing.size == new_portion.size && 
                            existing.price == new_portion.price &&
                            existing.timestamp == new_portion.timestamp) {
                            duplicate = true;
                            break;
                        }
                    }
                    
                    if (!duplicate) {
                        order.fill_portions.push_back(new_portion);
                    }
                }
                
                // Update execution percentage based on state first
                if (state == "filled") {
                    order.execution_percentage = 1.0;  // If order is filled, it's 100%
                    order.is_filled = true;
                } else {
                    // Calculate execution percentage based on filled size and original volume
                    // Make sure we have valid volume
                    if (order.volume > 0) {
                        order.execution_percentage = order.cumulative_filled_size / order.volume;
                    } else {
                        // If volume is 0, use filled size to determine if filled
                        order.execution_percentage = order.cumulative_filled_size > 0 ? 1.0 : 0.0;
                    }
                    order.is_filled = (order.execution_percentage >= 1.0);
                }
                
                std::cout << "[" << getCurrentTimestamp() << "] Updated order fill: "
                          << "OKX ID=" << okx_order_id
                          << " State ID=" << order.state_id
                          << " Side=" << order.side
                          << " Filled=" << order.cumulative_filled_size
                          << "/" << order.volume
                          << " (+" << fill_delta << " new)"
                          << " (" << std::fixed << std::setprecision(2) << order.execution_percentage * 100.0 << "%)"
                          << " State=" << state 
                          << " Volume=" << order.volume << std::endl;
            }
            break;
        }
    }
}

void OKXWebSocket::remove_filled_order(const std::string& okx_order_id) {
    // Note: orders_mutex_ should already be locked when this is called
    orders_.erase(
        std::remove_if(
            orders_.begin(), 
            orders_.end(),
            [&okx_order_id](const OrderInfo& order) {
                return order.okx_order_id == okx_order_id && order.is_filled;
            }
        ),
        orders_.end()
    );
} 