#include "../include/oms_handler.hpp"
#include <binary_utils.hpp>
#include <stdexcept>
#include <thread>
#include <nlohmann/json.hpp>

OMSHandler::OMSHandler(const std::string& host, int port,
                     const std::string& username, const std::string& password,
                     const std::string& okx_api_key,
                     const std::string& okx_secret_key,
                     const std::string& okx_passphrase)
    : host_(host), port_(port), username_(username), password_(password),
      is_running_(false), conn_(nullptr), socket_(nullptr) {
    
    // Initialize OKX WebSocket client
    okx_ws_ = std::make_unique<OKXWebSocket>(okx_api_key, okx_secret_key, okx_passphrase);
    
    // Initialize position size handler
    pos_size_handler_ = std::make_unique<PosSizeHandler>(20.0);  // 20% margin limit
    
    // Initialize trade state
    current_trade_.has_active_trade = false;
    current_trade_.is_long = false;
    current_trade_.size = 0.0;
    current_trade_.maxdd = 0.0;  // Initialize maxdd to 0
    current_trade_.cumulative_reward = 0.0;
    current_trade_.total_size = 0.0;

    // Set up the order ID callback for deque management
    okx_ws_->set_order_id_callback([this](uint32_t state_id, const std::string& okx_order_id) {
        okx_ws_->update_order_id(state_id, okx_order_id, true);
        std::cout << "Updated order ID for state " << state_id << ": " << okx_order_id << std::endl;
        known_orders_[okx_order_id] = state_id;
    });

    // Set up the order fill callback for trade management
    okx_ws_->set_order_fill_callback([this](const std::string& okx_order_id, 
                                          double filled_size,
                                          double avg_price,
                                          const std::string& side,
                                          const std::string& state,
                                          double pnl,
                                          int64_t fill_time) {
        std::cout << "\n[DEBUG] ========== Order Fill Callback Start ==========\n"
                  << "OKX Order ID: " << okx_order_id << "\n"
                  << "Filled Size: " << filled_size << "\n"
                  << "Avg Price: " << avg_price << "\n"
                  << "Side: " << side << "\n"
                  << "State: " << state << "\n"
                  << "PnL: " << pnl << "\n"
                  << "Fill Time: " << fill_time << std::endl;

        // First check if this order exists in our tracking
        bool order_exists = false;
        uint32_t state_id = 0;
        double sum_of_orders = 0.0;  // Moved to function scope
        {
            std::lock_guard<std::mutex> lock(okx_ws_->orders_mutex_);
            
            // First check known_orders_ map - this includes cancelled/moved orders
            auto known_it = known_orders_.find(okx_order_id);
            if (known_it != known_orders_.end()) {
                order_exists = true;
                state_id = known_it->second;
                std::cout << "[DEBUG] Found order in known_orders_ map with state ID: " << state_id 
                          << " (may be in cancellation queue)" << std::endl;
                
                // If order was in cancellation queue but got filled, we need to handle it
                bool in_active_deque = false;
                for (const auto& order : okx_ws_->orders_) {
                    if (order.okx_order_id == okx_order_id) {
                        in_active_deque = true;
                        break;
                    }
                }
                
                if (!in_active_deque && filled_size > 0) {
                    std::cout << "[INFO] Order " << okx_order_id << " was in cancellation queue but got filled. "
                              << "Moving back to active tracking." << std::endl;
                    
                    // Create new order info and add back to deque
                    OrderInfo recovered_order;
                    recovered_order.state_id = state_id;
                    recovered_order.okx_order_id = okx_order_id;
                    recovered_order.has_okx_id = true;
                    recovered_order.side = side;
                    recovered_order.filled_size = filled_size;
                    recovered_order.avg_fill_price = avg_price;
                    recovered_order.is_filled = (state == "filled");
                    recovered_order.order_state = state;
                    recovered_order.volume = filled_size;  // Set volume to filled size for recovered orders
                    recovered_order.price = avg_price;     // Use average fill price as the order price
                    recovered_order.execution_percentage = 1.0;  // Assume full execution for recovered orders
                    recovered_order.fill_time = fill_time;  // Track fill timestamp for execution sequence
                    
                    // Sort orders by fill time to maintain execution sequence
                    okx_ws_->orders_.push_back(recovered_order);
                    std::sort(okx_ws_->orders_.begin(), okx_ws_->orders_.end(),
                             [](const OrderInfo& a, const OrderInfo& b) {
                                 return a.fill_time < b.fill_time;
                             });
                    
                    std::cout << "[DEBUG] Recovered order details:\n"
                              << "  Volume: " << recovered_order.volume << "\n"
                              << "  Price: " << recovered_order.price << "\n"
                              << "  Filled Size: " << recovered_order.filled_size << "\n"
                              << "  Fill Time: " << recovered_order.fill_time << "\n"
                              << "  Execution %: " << (recovered_order.execution_percentage * 100.0) << "%" << std::endl;
                }
            }
            
            // If not found in known_orders_, check the deque
            if (!order_exists) {
                std::cout << "[DEBUG] Current orders in deque:" << std::endl;
                for (const auto& order : okx_ws_->orders_) {
                    std::cout << "  - Order ID: " << order.okx_order_id 
                              << ", State ID: " << order.state_id 
                              << ", State: " << order.order_state 
                              << ", Has OKX ID: " << (order.has_okx_id ? "Yes" : "No") << std::endl;
                    if (order.okx_order_id == okx_order_id) {
                        order_exists = true;
                        state_id = order.state_id;
                        known_orders_[okx_order_id] = state_id;  // Add to known orders
                        std::cout << "[DEBUG] Found order in deque with state ID: " << state_id << std::endl;
                        break;
                    }
                }
            }
        }

        // If we don't know about this order at all, ignore it
        if (!order_exists) {
            std::cout << "[WARNING] Order " << okx_order_id << " not found in tracking. This fill will be ignored." << std::endl;
            return;
        }

        // Update order in deque
        okx_ws_->update_order_fill(okx_order_id, filled_size, avg_price, side, state);

        // Get order details from deque
        bool was_partially_filled = false;
        double intended_volume = 0.0;
        double intended_price = 0.0;
        {
            std::lock_guard<std::mutex> lock(okx_ws_->orders_mutex_);
            for (const auto& order : okx_ws_->orders_) {
                if (order.okx_order_id == okx_order_id) {
                    was_partially_filled = (order.order_state == "partially_filled");
                    intended_volume = order.volume;
                    intended_price = order.price;
                    break;
                }
            }
        }

        std::cout << "[DEBUG] Order Details from Deque:\n"
                  << "Was Partially Filled: " << (was_partially_filled ? "Yes" : "No") << "\n"
                  << "Intended Volume: " << intended_volume << "\n"
                  << "Intended Price: " << intended_price << std::endl;

        std::cout << "[DEBUG] Current Orders in Trade:" << std::endl;
        for (const auto& order : current_trade_.orders) {
            std::cout << "  Order ID: " << order.okx_order_id 
                      << ", State ID: " << order.state_id
                      << ", Filled Size: " << order.filled_size
                      << ", Is Filled: " << (order.is_filled ? "Yes" : "No")
                      << ", Order State: " << order.order_state << std::endl;
        }

        bool need_balance_update = false;
        bool is_trade_closed = false;

        // Update trade state based on filled order
        if (!current_trade_.has_active_trade) {
            // Only create new trade if order has been filled and it's not a closing order
            if (filled_size > 0 && 
                (current_trade_.size == 0.0 || 
                 (current_trade_.is_long && side == "buy") || 
                 (!current_trade_.is_long && side == "sell"))) {
                
                current_trade_.has_active_trade = true;
                current_trade_.is_long = (side == "buy");
                current_trade_.size = filled_size;
                current_trade_.cumulative_reward = 0.0;
                current_trade_.total_size = 0.0;
                current_trade_.tradeId = okx_order_id;
                
                // Create and add the order
                OrderInfo order;
                order.state_id = state_id;
                order.okx_order_id = okx_order_id;
                order.filled_size = filled_size;
                order.avg_fill_price = avg_price;
                order.is_filled = (state == "filled");
                order.has_okx_id = true;
                order.order_state = state;
                order.volume = intended_volume;
                order.price = intended_price;
                order.side = side;
                order.tradeId = current_trade_.tradeId;
                    order.execution_percentage = (order.volume > 0) ? (filled_size / order.volume) : 0.0;

                // Add fill portion
                OrderInfo::FillPortion new_portion;
                new_portion.tradeId = current_trade_.tradeId;
                new_portion.size = filled_size;
                new_portion.price = avg_price;
                new_portion.timestamp = fill_time;
                new_portion.is_closing = false;
                order.fill_portions.push_back(new_portion);
                
                current_trade_.orders.push_back(order);
                
                // Only publish after adding to trade struct
                publishTradeUpdate(state_id, okx_order_id);
                
                std::cout << "[" << getCurrentTimestamp() << "] New trade opened: " 
                          << (current_trade_.is_long ? "LONG" : "SHORT") 
                          << " Size: " << current_trade_.size << std::endl;
                need_balance_update = true;
            }
        } else {
            bool is_same_direction = (current_trade_.is_long && side == "buy") || 
                                   (!current_trade_.is_long && side == "sell");
            
            std::cout << "[DEBUG] Processing order for existing trade:\n"
                      << "  Trade ID: " << current_trade_.tradeId << "\n"
                      << "  Order ID: " << okx_order_id << "\n"
                      << "  Is Same Direction: " << (is_same_direction ? "Yes" : "No") << std::endl;
            
            if (is_same_direction) {
                // Find previous filled size for this order
                double previous_filled_size = 0.0;
                for (const auto& order : current_trade_.orders) {
                    if (order.okx_order_id == okx_order_id) {
                        previous_filled_size = order.filled_size;
                        break;
                    }
                }
                
                // Calculate the new fill amount (delta)
                double fill_delta = filled_size - previous_filled_size;
                
                // Update position size - add for same direction
                double previous_size = current_trade_.size;
                current_trade_.size = current_trade_.is_long ? 
                    previous_size + fill_delta : 
                    -(std::abs(previous_size) + fill_delta);  // Ensure proper sign for SHORT
                
                std::cout << "[DEBUG] Processing Same Direction Order\n"
                          << "Previous Filled Size Found: " << previous_filled_size << "\n"
                          << "Current Filled Size: " << filled_size << "\n"
                          << "Calculated Fill Delta: " << fill_delta << std::endl;
                
                // Add or update order in trade struct first
                bool order_found = false;
                for (auto& order : current_trade_.orders) {
                    if (order.okx_order_id == okx_order_id) {
                        std::cout << "[DEBUG] Updating existing order in trade:\n"
                                  << "  Trade ID: " << order.tradeId << "\n"
                                  << "  Order ID: " << order.okx_order_id << "\n"
                                  << "  Previous Fill: " << order.filled_size << "\n"
                                  << "  New Fill: " << filled_size << std::endl;
                        
                        // If this is the first fill for this order, add the initial portion
                        if (order.fill_portions.empty() && previous_filled_size > 0) {
                            OrderInfo::FillPortion initial_portion;
                            initial_portion.tradeId = current_trade_.tradeId;
                            initial_portion.size = previous_filled_size;
                            initial_portion.price = order.avg_fill_price;
                            initial_portion.timestamp = fill_time - 1;  // Ensure correct ordering
                            initial_portion.is_closing = false;
                            order.fill_portions.push_back(initial_portion);
                        }
                        
                        // Add new fill portion for this trade
                        OrderInfo::FillPortion new_portion;
                        new_portion.tradeId = current_trade_.tradeId;
                        new_portion.size = fill_delta;
                        new_portion.price = avg_price;
                        new_portion.timestamp = fill_time;
                        new_portion.is_closing = false;  // Same direction fills are never closing
                        order.fill_portions.push_back(new_portion);
                        
                        order.filled_size = filled_size;
                        order.avg_fill_price = avg_price;
                        order.order_state = state;
                        order.side = side;
                        // Update execution percentage based on state first
                        if (state == "filled") {
                            order.execution_percentage = 1.0;  // If order is filled, it's 100%
                            order.is_filled = true;
                        } else {
                            // Only update execution percentage if not filled
                            order.execution_percentage = (order.volume > 0) ? (filled_size / order.volume) : 0.0;
                            order.is_filled = false;
                        }
                        
                        std::cout << "[DEBUG] After update:\n"
                                  << "  Filled Size: " << order.filled_size
                                  << ", State: " << order.order_state
                                  << ", Execution %: " << order.execution_percentage
                                  << "\n  Fill Portions:" << std::endl;
                        for (const auto& portion : order.fill_portions) {
                            std::cout << "    Trade ID: " << portion.tradeId
                                      << ", Size: " << portion.size
                                      << ", Price: " << portion.price << std::endl;
                        }
                        order_found = true;
                        break;
                    }
                }
                
                if (!order_found && filled_size > 0) {
                    OrderInfo order;
                    order.state_id = state_id;
                    order.okx_order_id = okx_order_id;
                    order.filled_size = filled_size;
                    order.avg_fill_price = avg_price;
                    order.is_filled = (state == "filled");
                    order.has_okx_id = true;
                    order.order_state = state;
                    order.volume = intended_volume;
                    order.price = intended_price;
                    order.side = side;
                    order.tradeId = current_trade_.tradeId;  // Ensure trade ID is set
                    order.execution_percentage = (order.volume > 0) ? (filled_size / order.volume) : 0.0;
                    
                    // Add fill portion for new order
                    OrderInfo::FillPortion new_portion;
                    new_portion.tradeId = current_trade_.tradeId;
                    new_portion.size = filled_size;  // Use entire filled size for new order
                    new_portion.price = avg_price;
                    new_portion.timestamp = fill_time;
                    new_portion.is_closing = false;  // Same direction orders are never closing
                    order.fill_portions.push_back(new_portion);
                    
                    std::cout << "[DEBUG] Adding new order to trade struct:\n"
                              << "  Trade ID: " << order.tradeId << "\n"
                              << "  Order ID: " << order.okx_order_id << "\n"
                              << "  State ID: " << order.state_id << "\n"
                              << "  Side: " << order.side << "\n"
                              << "  Filled Size: " << order.filled_size << "\n"
                              << "  Price: " << order.price << "\n"
                              << "  Fill Portions:" << std::endl;
                    for (const auto& portion : order.fill_portions) {
                        std::cout << "    Trade ID: " << portion.tradeId
                                  << ", Size: " << portion.size
                                  << ", Price: " << portion.price
                                  << ", Is Closing: " << (portion.is_closing ? "Yes" : "No") << std::endl;
                    }
                    current_trade_.orders.push_back(order);
                }
                
                // After updating orders, verify the state
                std::cout << "[DEBUG] After Order Update:\n"
                          << "Order Found: " << (order_found ? "Yes" : "No") << "\n"
                          << "Current Size: " << current_trade_.size << "\n"
                          << "Updated Orders:" << std::endl;
                for (const auto& order : current_trade_.orders) {
                    std::cout << "  Order ID: " << order.okx_order_id 
                              << ", Filled Size: " << order.filled_size
                              << ", Execution %: " << order.execution_percentage << std::endl;
                }
                
                // Recalculate current size based on orders
                sum_of_orders = 0.0;
                double total_closed_size = 0.0;
                double total_buy_size = 0.0;
                double total_sell_size = 0.0;
                
                // First calculate total buy and sell sizes from all portions
                for (const auto& order : current_trade_.orders) {
                    for (const auto& portion : order.fill_portions) {
                        if (portion.tradeId == current_trade_.tradeId) {
                            if (order.side == "buy") {
                                total_buy_size += portion.size;
                            } else {
                                total_sell_size += portion.size;
                            }
                        }
                    }
                }
                
                // Calculate net position
                sum_of_orders = total_buy_size - total_sell_size;
                
                std::cout << "[DEBUG] Position calculation details:\n"
                          << "  Total buy size: " << total_buy_size << "\n"
                          << "  Total sell size: " << total_sell_size << "\n"
                          << "  Net position (sum_of_orders): " << sum_of_orders << std::endl;
                
                // Only consider the trade closed if the net position is effectively zero
                if (std::abs(sum_of_orders) < 1e-8) {
                    is_trade_closed = true;
                    current_trade_.size = 0.0;
                } else {
                    // Update the current trade size based on net position
                    current_trade_.size = sum_of_orders;
                    is_trade_closed = false;
                }

                std::cout << "[DEBUG] Trade status after calculation:\n"
                          << "  Is trade closed: " << (is_trade_closed ? "Yes" : "No") << "\n"
                          << "  Current size: " << current_trade_.size << "\n"
                          << "  Is long: " << (current_trade_.is_long ? "Yes" : "No") << std::endl;
                
                // Only update size if there's a significant difference and we're not closing the position
                if (std::abs(current_trade_.size) >= 1e-8 && 
                    std::abs(current_trade_.size - sum_of_orders) > 1e-8 &&
                    !is_same_direction) {  // Don't correct size during position flips
                    
                    // Ensure we maintain the correct sign based on position direction
                    double corrected_size = sum_of_orders;
                    if (!current_trade_.is_long && corrected_size > 0) {
                        corrected_size = -corrected_size;
                    }
                    
                    std::cout << "[DEBUG] Size calculation details:\n"
                              << "  Current size: " << current_trade_.size << "\n"
                              << "  Calculated sum: " << sum_of_orders << "\n"
                              << "  Is position flip: " << (is_same_direction ? "Yes" : "No") << std::endl;
                    
                    if (std::abs(current_trade_.size - corrected_size) > 1e-8) {
                    std::cout << "[WARNING] Size mismatch detected. Correcting size from " 
                                  << current_trade_.size << " to " << corrected_size << std::endl;
                        current_trade_.size = corrected_size;
                    }
                }

                // Update cumulative reward and total size for position reduction
                if (pnl != 0.0 && filled_size > 0 && avg_price > 0) {
                    std::cout << "\033[1;36m[REWARD DEBUG] Starting reward calculation:\033[0m\n"
                              << "  PnL: " << pnl << " USDT\n"
                              << "  Filled Size: " << filled_size << " contracts\n"
                              << "  Avg Price: " << avg_price << " USDT\n"
                              << "  Previous Filled: " << previous_filled_size << " contracts\n"
                              << "  Current Cumulative Reward: " << current_trade_.cumulative_reward << "\n"
                              << "  Current Total Size: " << current_trade_.total_size << std::endl;

                    double pnl_percentage = pnl / (filled_size * avg_price);
                    if (std::isfinite(pnl_percentage)) {
                        // Only add the new fill amount to total_size
                        double previous_filled = 0.0;
                        for (const auto& order : current_trade_.orders) {
                            if (order.okx_order_id == okx_order_id) {
                                previous_filled = order.filled_size;
                                break;
                            }
                        }
                        double new_fill_amount = filled_size - previous_filled;
                        double reward_increment = new_fill_amount * pnl_percentage;
                        
                        std::cout << "\033[1;36m[REWARD DEBUG] Calculation details:\033[0m\n"
                                  << "  PnL Percentage: " << (pnl_percentage * 100.0) << "%\n"
                                  << "  New Fill Amount: " << new_fill_amount << " contracts\n"
                                  << "  Reward Increment: " << reward_increment << "\n"
                                  << "  MaxDD: " << okx_ws_->get_maxdd() << std::endl;

                        current_trade_.cumulative_reward += reward_increment;
                        current_trade_.total_size += new_fill_amount;

                        std::cout << "\033[1;36m[REWARD DEBUG] Updated values:\033[0m\n"
                                  << "  New Cumulative Reward: " << current_trade_.cumulative_reward << "\n"
                                  << "  New Total Size: " << current_trade_.total_size << std::endl;
                    } else {
                        std::cout << "\033[1;31m[REWARD DEBUG] Warning: Invalid PnL percentage calculation\033[0m\n"
                                  << "  PnL: " << pnl << "\n"
                                  << "  Filled Size: " << filled_size << "\n"
                                  << "  Avg Price: " << avg_price << std::endl;
                    }
                }

                // Update cumulative prices and sizes based on order side
                std::string current_side;
                for (const auto& current_order : current_trade_.orders) {
                    if (current_order.okx_order_id == okx_order_id) {
                        current_side = current_order.side;
                        if (current_order.side == "buy") {
                            current_trade_.buy_side_cumulative_price += filled_size * avg_price;
                            current_trade_.buy_side_total_size += filled_size;
                            
                            std::cout << "\033[1;36m[PRICE DEBUG] Updated buy side averages:\033[0m\n"
                                      << "  New fill: " << filled_size << " @ " << avg_price << "\n"
                                      << "  Cumulative price sum: " << current_trade_.buy_side_cumulative_price << "\n"
                                      << "  Total buy size: " << current_trade_.buy_side_total_size << "\n"
                                      << "  Average buy price: " << current_trade_.get_avg_buy_price() << std::endl;
                        } else {
                            current_trade_.sell_side_cumulative_price += filled_size * avg_price;
                            current_trade_.sell_side_total_size += filled_size;
                            
                            std::cout << "\033[1;36m[PRICE DEBUG] Updated sell side averages:\033[0m\n"
                                      << "  New fill: " << filled_size << " @ " << avg_price << "\n"
                                      << "  Cumulative price sum: " << current_trade_.sell_side_cumulative_price << "\n"
                                      << "  Total sell size: " << current_trade_.sell_side_total_size << "\n"
                                      << "  Average sell price: " << current_trade_.get_avg_sell_price() << std::endl;
                        }
                        break;
                    }
                }

                // Check if trade should be closed due to size reaching 0
                if (std::abs(current_trade_.size) < 1e-8) {
                    is_trade_closed = true;
                    
                    // Calculate final reward using new formula
                    double final_reward = 0.0;
                    double avg_buy_price = current_trade_.get_avg_buy_price();
                    double avg_sell_price = current_trade_.get_avg_sell_price();
                    
                    std::cout << "\033[1;36m[REWARD DEBUG] Calculating final reward for closed trade:\033[0m\n"
                              << "  Average Buy Price: " << avg_buy_price << "\n"
                              << "  Average Sell Price: " << avg_sell_price << "\n"
                              << "  Trade Direction: " << (current_trade_.is_long ? "LONG" : "SHORT") << "\n"
                              << "  MaxDD: " << okx_ws_->get_maxdd() << std::endl;
                    
                    if (avg_buy_price > 0 && avg_sell_price > 0) {
                        if (current_trade_.is_long) {
                            // For long trades: ((sellprice-buyprice) / buyprice) * 100 * 100
                            final_reward = ((avg_sell_price - avg_buy_price) / avg_buy_price) * 100.0 * 100.0;
                        } else {
                            // For short trades: ((buyprice-sellprice) / sellprice) * 100 * 100
                            final_reward = ((avg_buy_price - avg_sell_price) / avg_sell_price) * 100.0 * 100.0;
                        }
                        
                        std::cout << "\033[1;36m[REWARD DEBUG] Base reward calculated:\033[0m\n"
                                  << "  Base Reward: " << final_reward << "%" << std::endl;
                        
                        // Apply MaxDD adjustment
                        if (final_reward > 0) {
                            final_reward *= (1.0 - 2.0 * std::abs(okx_ws_->get_maxdd()));
                            std::cout << "\033[1;36m[REWARD DEBUG] Positive reward adjusted for MaxDD:\033[0m\n"
                                      << "  MaxDD Multiplier: " << (1.0 - 2.0 * std::abs(okx_ws_->get_maxdd())) << "\n"
                                      << "  Final Reward: " << final_reward << "%" << std::endl;
                        } else if (final_reward < 0) {
                            final_reward *= (1.0 + 2.0 * std::abs(okx_ws_->get_maxdd()));
                            std::cout << "\033[1;36m[REWARD DEBUG] Negative reward adjusted for MaxDD:\033[0m\n"
                                      << "  MaxDD Multiplier: " << (1.0 + 2.0 * std::abs(okx_ws_->get_maxdd())) << "\n"
                                      << "  Final Reward: " << final_reward << "%" << std::endl;
                        }
                    }
                    
                    std::cout << "\033[1;36m[REWARD DEBUG] Trade closure summary:\033[0m\n"
                              << "  Initial Size: " << previous_size << "\n"
                              << "  Total Buy Size: " << current_trade_.buy_side_total_size << "\n"
                              << "  Total Sell Size: " << current_trade_.sell_side_total_size << "\n"
                              << "  Average Buy Price: " << avg_buy_price << "\n"
                              << "  Average Sell Price: " << avg_sell_price << "\n"
                              << "  Final Reward: " << final_reward << "%\n"
                              << "  Maximum Drawdown: " << okx_ws_->get_maxdd() << std::endl;
                    
                    // First ensure the closing order is in current_trade_.orders
                    bool closing_order_found = false;
                    for (const auto& order : current_trade_.orders) {
                        if (order.okx_order_id == okx_order_id) {
                            closing_order_found = true;
                            break;
                        }
                    }
                    
                    if (!closing_order_found) {
                        // Calculate closing and opening sizes based on the position analysis that was already done
                        double closing_size = std::min(fill_delta, std::abs(previous_size));
                        double opening_size = fill_delta - closing_size;

                        // First handle the closing portion
                        OrderInfo closing_order;
                        closing_order.state_id = state_id;
                        closing_order.okx_order_id = okx_order_id;
                        closing_order.filled_size = closing_size;  // Only use closing size here
                        closing_order.avg_fill_price = avg_price;
                        closing_order.is_filled = (state == "filled");
                        closing_order.has_okx_id = true;
                        closing_order.order_state = state;
                        closing_order.volume = intended_volume;
                        closing_order.price = intended_price;
                        closing_order.side = side;
                        closing_order.tradeId = current_trade_.tradeId;
                        
                        // Calculate execution percentage for closing portion
                        closing_order.execution_percentage = (closing_order.volume > 0) ? 
                            (closing_size / closing_order.volume) : 0.0;  // Store as decimal (0-1)
                        
                        // Add fill portion for closing
                        if (closing_size >= 0.001) {
                            OrderInfo::FillPortion closing_portion;
                            closing_portion.tradeId = current_trade_.tradeId;
                            closing_portion.size = closing_size;
                            closing_portion.price = avg_price;
                            closing_portion.timestamp = fill_time;
                            closing_portion.is_closing = true;
                            closing_portion.execution_percentage = closing_order.execution_percentage;  // Keep as decimal
                            closing_order.fill_portions.push_back(closing_portion);
                        }
                        
                        // Add closing order to current trade
                        current_trade_.orders.push_back(closing_order);

                        // Publish execution update for the closing order
                        publishTradeUpdate(state_id, okx_order_id, closing_order.execution_percentage);

                        // Now handle the opening portion if it exists
                        if (opening_size >= 0.001) {
                            // Create a new trade for the opening portion
                            Trade new_trade;
                            new_trade.has_active_trade = true;
                            new_trade.is_long = (side == "buy");
                            new_trade.size = opening_size;
                            new_trade.tradeId = okx_order_id;  // Use order ID as new trade ID
                            
                            // Create opening order
                            OrderInfo opening_order;
                            opening_order.state_id = state_id;
                            opening_order.okx_order_id = okx_order_id;
                            opening_order.filled_size = opening_size;
                            opening_order.avg_fill_price = avg_price;
                            opening_order.is_filled = (state == "filled");
                            opening_order.has_okx_id = true;
                            opening_order.order_state = state;
                            opening_order.volume = intended_volume;
                            opening_order.price = intended_price;
                            opening_order.side = side;
                            opening_order.tradeId = new_trade.tradeId;
                            
                            // Calculate execution percentage for opening portion
                            opening_order.execution_percentage = (opening_order.volume > 0) ? 
                                (opening_size / opening_order.volume) : 0.0;  // Store as decimal (0-1)
                            
                            // Add fill portion for opening
                            OrderInfo::FillPortion opening_portion;
                            opening_portion.tradeId = new_trade.tradeId;
                            opening_portion.size = opening_size;
                            opening_portion.price = avg_price;
                            opening_portion.timestamp = fill_time;
                            opening_portion.is_closing = false;
                            opening_portion.execution_percentage = (opening_order.volume > 0) ? 
                                (opening_size / opening_order.volume) : 0.0;  // Store as decimal (0-1)
                            opening_order.fill_portions.push_back(opening_portion);
                            
                            // Add opening order to new trade
                            new_trade.orders.push_back(opening_order);
                            
                            // Store new trade info for later use
                            next_trade_ = new_trade;
                            has_next_trade_ = true;

                            // Publish execution update for the opening portion
                            publishTradeUpdate(state_id, okx_order_id, opening_order.execution_percentage);
                        }
                        
                        std::cout << "[DEBUG] Processed dual-purpose order:\n"
                                  << "  Order ID: " << okx_order_id 
                                  << ", Total Size: " << filled_size 
                                  << ", Side: " << side << "\n"
                                  << "  Closing Portion: " << closing_size 
                                  << " (Trade ID: " << current_trade_.tradeId 
                                  << ", Execution: " << std::fixed << std::setprecision(2) 
                                  << (closing_size / intended_volume * 100.0) << "%)\n";  // Convert to percentage only for display
                        
                        if (opening_size >= 0.001) {
                            std::cout << "  Opening Portion: " << opening_size 
                                      << " (Trade ID: " << okx_order_id 
                                      << ", Execution: " << std::fixed << std::setprecision(2)
                                      << (opening_size / intended_volume * 100.0) << "%)\n"  // Convert to percentage only for display
                                      << "  New Trade Created: Yes (Size: " << opening_size << ")" << std::endl;
                        }
                    }
                    
                    // Prepare filled portions for trade closure message
                    std::vector<std::pair<std::string, double>> filled_portions;
                    for (const auto& order : current_trade_.orders) {
                        // For dual-purpose orders, add separate entries for each portion
                        if (!order.fill_portions.empty()) {
                            for (const auto& portion : order.fill_portions) {
                                // Convert decimal to percentage (0-100) for output
                                double exec_pct = std::min(100.0, portion.execution_percentage * 100.0);
                                filled_portions.push_back(std::make_pair(
                                    order.okx_order_id,
                                    exec_pct
                                ));
                            }
                        } else {
                            // Convert decimal to percentage (0-100) for output
                            double exec_pct = std::min(100.0, order.execution_percentage * 100.0);
                            filled_portions.push_back(std::make_pair(
                                order.okx_order_id,
                                exec_pct
                            ));
                        }
                    }

                    // Publish trade closure update
                    publishTradeUpdate(state_id, okx_order_id, true, filled_portions);
                    
                    printTradeOrders();
                    
                    // Store current trade ID before clearing
                    std::string prev_trade_id = current_trade_.tradeId;
                    
                    // If we have a next trade ready, switch to it
                    if (has_next_trade_) {
                        current_trade_ = next_trade_;
                        has_next_trade_ = false;
                    } else {
                        // Otherwise clear the trade struct
                        current_trade_ = Trade();
                    }
                    
                    std::cout << "[DEBUG] Trade transition completed:\n"
                              << "  Previous Trade ID: " << prev_trade_id << "\n"
                              << "  New Trade ID: " << current_trade_.tradeId << "\n"
                              << "  New Position Size: " << current_trade_.size << std::endl;
                    
                    need_balance_update = true;
                    return;  // Exit immediately after trade closure
                } else {
                    // For position reduction without closure, publish after updating trade struct
                    if (filled_size > 0) {  // Changed from closing_size to filled_size
                        publishTradeUpdate(state_id, okx_order_id);
                    }
                }
            } else {
                double previous_size = current_trade_.size;
                bool previous_is_long = current_trade_.is_long;
                
                // Find previous filled size for this order
                double previous_filled = 0.0;
                for (const auto& order : current_trade_.orders) {
                    if (order.okx_order_id == okx_order_id) {
                        previous_filled = order.filled_size;
                        break;
                    }
                }
                
                // Calculate the new fill amount (delta)
                double fill_delta = filled_size - previous_filled;
                
                // Calculate closing and opening sizes
                double closing_size = std::min(fill_delta, std::abs(previous_size));
                double opening_size = fill_delta - closing_size;
                
                std::cout << "\n[DEBUG] Position Flip/Close Analysis:\n"
                          << "  Previous Position Size: " << previous_size << "\n"
                          << "  Fill Delta: " << fill_delta << "\n"
                          << "  Closing Size: " << closing_size << "\n"
                          << "  Opening Size: " << opening_size << "\n"
                          << "  Is Exact Close: " << (std::abs(closing_size - fill_delta) < 1e-8 ? "Yes" : "No") << std::endl;
                
                // Detect if this order will flip the position
                bool is_position_flip = (opening_size >= 0.001) && 
                    ((previous_is_long && side == "sell") || (!previous_is_long && side == "buy"));
                
                // First close the existing position
                current_trade_.size = (previous_size > 0) ? 
                    std::max(0.0, previous_size - closing_size) : 
                    std::min(0.0, previous_size + closing_size);
                
                std::cout << "[DEBUG] After Position Close:\n"
                          << "  New Position Size: " << current_trade_.size << "\n"
                          << "  Is Position Flip: " << (is_position_flip ? "Yes" : "No") << std::endl;

                // Update total reduced size with closing portion
                current_trade_.total_size += closing_size;
                
                // If we closed the entire position, trigger position closure
                if (std::abs(current_trade_.size) < 1e-8) {
                    std::cout << "[DEBUG] Position Closure Detected:\n"
                              << "  Opening Size: " << opening_size << "\n"
                              << "  Intended Volume: " << intended_volume << "\n"
                              << "  Closing Size: " << closing_size << "\n"
                              << "  Should Create New Trade: " << (opening_size >= 0.001 && std::abs(closing_size - intended_volume) > 1e-8 ? "Yes" : "No") << std::endl;
                    
                    // First add the closing portion to the current trade
                    bool closing_order_found = false;
                    for (const auto& order : current_trade_.orders) {
                        if (order.okx_order_id == okx_order_id) {
                            closing_order_found = true;
                            break;
                        }
                    }
                    
                    if (!closing_order_found) {
                        // Calculate closing and opening sizes based on the position analysis that was already done
                        double closing_size = std::min(fill_delta, std::abs(previous_size));
                        double opening_size = fill_delta - closing_size;

                        // First handle the closing portion
                        OrderInfo closing_order;
                        closing_order.state_id = state_id;
                        closing_order.okx_order_id = okx_order_id;
                        closing_order.filled_size = closing_size;  // Only use closing size here
                        closing_order.avg_fill_price = avg_price;
                        closing_order.is_filled = (state == "filled");
                        closing_order.has_okx_id = true;
                        closing_order.order_state = state;
                        closing_order.volume = intended_volume;
                        closing_order.price = intended_price;
                        closing_order.side = side;
                        closing_order.tradeId = current_trade_.tradeId;
                        
                        // Calculate execution percentage for closing portion
                        closing_order.execution_percentage = (closing_order.volume > 0) ? 
                            (closing_size / closing_order.volume) : 0.0;  // Store as decimal (0-1)
                        
                        // Add fill portion for closing
                        if (closing_size >= 0.001) {
                            OrderInfo::FillPortion closing_portion;
                            closing_portion.tradeId = current_trade_.tradeId;
                            closing_portion.size = closing_size;
                            closing_portion.price = avg_price;
                            closing_portion.timestamp = fill_time;
                            closing_portion.is_closing = true;
                            closing_portion.execution_percentage = closing_order.execution_percentage;  // Keep as decimal
                            closing_order.fill_portions.push_back(closing_portion);
                        }
                        
                        // Add closing order to current trade
                        current_trade_.orders.push_back(closing_order);

                        // Publish execution update for the closing order
                        publishTradeUpdate(state_id, okx_order_id, closing_order.execution_percentage);

                        // Now handle the opening portion if it exists
                        if (opening_size >= 0.001) {
                            // Create a new trade for the opening portion
                            Trade new_trade;
                            new_trade.has_active_trade = true;
                            new_trade.is_long = (side == "buy");
                            new_trade.size = opening_size;
                            new_trade.tradeId = okx_order_id;  // Use order ID as new trade ID
                            
                            // Create opening order
                            OrderInfo opening_order;
                            opening_order.state_id = state_id;
                            opening_order.okx_order_id = okx_order_id;
                            opening_order.filled_size = opening_size;
                            opening_order.avg_fill_price = avg_price;
                            opening_order.is_filled = (state == "filled");
                            opening_order.has_okx_id = true;
                            opening_order.order_state = state;
                            opening_order.volume = intended_volume;
                            opening_order.price = intended_price;
                            opening_order.side = side;
                            opening_order.tradeId = new_trade.tradeId;
                            
                            // Calculate execution percentage for opening portion
                            opening_order.execution_percentage = (opening_order.volume > 0) ? 
                                (opening_size / opening_order.volume) : 0.0;  // Store as decimal (0-1)
                            
                            // Add fill portion for opening
                            OrderInfo::FillPortion opening_portion;
                            opening_portion.tradeId = new_trade.tradeId;
                            opening_portion.size = opening_size;
                            opening_portion.price = avg_price;
                            opening_portion.timestamp = fill_time;
                            opening_portion.is_closing = false;
                            opening_portion.execution_percentage = (opening_order.volume > 0) ? 
                                (opening_size / opening_order.volume) : 0.0;  // Store as decimal (0-1)
                            opening_order.fill_portions.push_back(opening_portion);
                            
                            // Add opening order to new trade
                            new_trade.orders.push_back(opening_order);
                            
                            // Store new trade info for later use
                            next_trade_ = new_trade;
                            has_next_trade_ = true;

                            // Publish execution update for the opening portion
                            publishTradeUpdate(state_id, okx_order_id, opening_order.execution_percentage);
                        }
                        
                        std::cout << "[DEBUG] Processed dual-purpose order:\n"
                                  << "  Order ID: " << okx_order_id 
                                  << ", Total Size: " << filled_size 
                                  << ", Side: " << side << "\n"
                                  << "  Closing Portion: " << closing_size 
                                  << " (Trade ID: " << current_trade_.tradeId 
                                  << ", Execution: " << std::fixed << std::setprecision(2) 
                                  << (closing_size / intended_volume * 100.0) << "%)\n";  // Convert to percentage only for display
                        
                        if (opening_size >= 0.001) {
                            std::cout << "  Opening Portion: " << opening_size 
                                      << " (Trade ID: " << okx_order_id 
                                      << ", Execution: " << std::fixed << std::setprecision(2)
                                      << (opening_size / intended_volume * 100.0) << "%)\n"  // Convert to percentage only for display
                                      << "  New Trade Created: Yes (Size: " << opening_size << ")" << std::endl;
                        }
                    }
                    
                    // Prepare filled portions for trade closure message
                    std::vector<std::pair<std::string, double>> filled_portions;
                    for (const auto& order : current_trade_.orders) {
                        // For dual-purpose orders, add separate entries for each portion
                        if (!order.fill_portions.empty()) {
                            for (const auto& portion : order.fill_portions) {
                                // Convert decimal to percentage (0-100) for output
                                double exec_pct = std::min(100.0, portion.execution_percentage * 100.0);
                                filled_portions.push_back(std::make_pair(
                                    order.okx_order_id,
                                    exec_pct
                                ));
                            }
                        } else {
                            // Convert decimal to percentage (0-100) for output
                            double exec_pct = std::min(100.0, order.execution_percentage * 100.0);
                            filled_portions.push_back(std::make_pair(
                                order.okx_order_id,
                                exec_pct
                            ));
                        }
                    }

                    // Publish trade closure update
                    publishTradeUpdate(state_id, okx_order_id, true, filled_portions);
                    
                    printTradeOrders();
                    
                    // Store current trade ID before clearing
                    std::string prev_trade_id = current_trade_.tradeId;
                    
                    // If we have a next trade ready, switch to it
                    if (has_next_trade_) {
                        current_trade_ = next_trade_;
                        has_next_trade_ = false;
                    } else {
                        // Otherwise clear the trade struct
                        current_trade_ = Trade();
                    }
                    
                    std::cout << "[DEBUG] Trade transition completed:\n"
                              << "  Previous Trade ID: " << prev_trade_id << "\n"
                              << "  New Trade ID: " << current_trade_.tradeId << "\n"
                              << "  New Position Size: " << current_trade_.size << std::endl;
                    
                    need_balance_update = true;
                    return;  // Exit immediately after trade closure
                } else if (opening_size >= 0.001) {
                    // If we still have a position and need to open in opposite direction
                    current_trade_.is_long = (side == "buy");  // Set direction based on order side
                    current_trade_.size = current_trade_.is_long ? opening_size : -opening_size;
                }
                
                // Debug logging for size verification
                std::cout << "[DEBUG] Position Size Update:\n"
                          << "  Previous Size: " << previous_size << "\n"
                          << "  Previous Filled: " << previous_filled << "\n"
                          << "  New Fill Delta: " << fill_delta << "\n"
                          << "  Closing Size: " << closing_size << "\n"
                          << "  Opening Size: " << opening_size << "\n"
                          << "  New Size: " << current_trade_.size << "\n"
                          << "  New Direction: " << (current_trade_.is_long ? "LONG" : "SHORT") << std::endl;
                
                // Add or update order in trade struct first
                bool order_found = false;
                for (auto& order : current_trade_.orders) {
                    if (order.okx_order_id == okx_order_id) {
                        // For position flips, use opening_size instead of total filled_size
                        order.filled_size = (opening_size >= 0.001) ? opening_size : filled_size;
                        order.avg_fill_price = avg_price;
                        order.order_state = state;
                        order.side = side;
                        order.tradeId = current_trade_.tradeId;
                        // Calculate execution percentage based on opening portion vs total volume for flips
                        order.execution_percentage = (order.volume > 0) ? 
                            ((opening_size >= 0.001) ? (opening_size / order.volume) : (filled_size / order.volume)) : 0.0;
                        
                        // Add fill portions for both closing and opening if applicable
                        if (closing_size >= 0.001) {
                            OrderInfo::FillPortion closing_portion_obj;
                            closing_portion_obj.tradeId = current_trade_.tradeId;
                            closing_portion_obj.size = closing_size;
                            closing_portion_obj.price = avg_price;
                            closing_portion_obj.timestamp = fill_time;
                            closing_portion_obj.is_closing = true;  // Mark as closing portion
                            order.fill_portions.push_back(closing_portion_obj);
                        }
                        
                        if (opening_size >= 0.001) {
                            OrderInfo::FillPortion opening_portion_obj;
                            opening_portion_obj.tradeId = current_trade_.tradeId;
                            opening_portion_obj.size = opening_size;
                            opening_portion_obj.price = avg_price;
                            opening_portion_obj.timestamp = fill_time;
                            opening_portion_obj.is_closing = false;  // Mark as opening portion
                            order.fill_portions.push_back(opening_portion_obj);
                        }
                        
                        order_found = true;
                        break;
                    }
                }
                
                if (!order_found && fill_delta > 0) {  // Only create new order if we have new fills
                    // Check if this is a pure closing order (no opening portion)
                    if (opening_size < 0.001 && closing_size > 0) {
                        // Create a new order for tracking the closing portion
                        OrderInfo closing_order;
                        closing_order.state_id = state_id;
                        closing_order.okx_order_id = okx_order_id;
                        closing_order.filled_size = filled_size;
                        closing_order.avg_fill_price = avg_price;
                        closing_order.is_filled = (state == "filled");
                        closing_order.has_okx_id = true;
                        closing_order.order_state = state;
                        closing_order.volume = intended_volume;
                        closing_order.price = intended_price;
                        closing_order.side = side;
                        closing_order.tradeId = current_trade_.tradeId;
                        closing_order.execution_percentage = (closing_order.volume > 0) ? 
                            (filled_size / closing_order.volume) : 0.0;
                        
                        // Add fill portion for the closing portion
                        OrderInfo::FillPortion closing_portion;
                        closing_portion.tradeId = current_trade_.tradeId;
                        closing_portion.size = filled_size;  // Use filled_size instead of closing_size
                        closing_portion.price = avg_price;
                        closing_portion.timestamp = fill_time;
                        closing_portion.is_closing = true;
                        closing_order.fill_portions.push_back(closing_portion);
                        
                        current_trade_.orders.push_back(closing_order);
                        std::cout << "[DEBUG] Added pure closing order to trade struct:\n"
                                  << "  Order ID: " << closing_order.okx_order_id 
                                  << ", Size: " << closing_order.filled_size 
                                  << ", Side: " << closing_order.side << std::endl;
                    }
                }
                
                // Recalculate current size based on orders
                sum_of_orders = 0.0;
                double total_buy_size = 0.0;
                double total_sell_size = 0.0;
                
                std::cout << "[DEBUG] Fill Portions Analysis:" << std::endl;
                // Calculate total buy and sell sizes from all portions
                for (const auto& order : current_trade_.orders) {
                    std::cout << "  Order " << order.okx_order_id << " Fill Portions:" << std::endl;
                    for (const auto& portion : order.fill_portions) {
                        std::cout << "    - Trade ID: " << portion.tradeId 
                                  << ", Size: " << portion.size 
                                  << ", Side: " << order.side
                                  << ", Is Closing: " << (portion.is_closing ? "Yes" : "No") << std::endl;
                        
                        if (portion.tradeId == current_trade_.tradeId) {
                            if (order.side == "buy") {
                                total_buy_size += portion.size;
                            } else {
                                total_sell_size += portion.size;
                            }
                        }
                    }
                }
                
                // Calculate net position
                sum_of_orders = total_buy_size - total_sell_size;
                
                std::cout << "[DEBUG] Position Size Calculation:\n"
                          << "  Total Buy Size: " << total_buy_size << "\n"
                          << "  Total Sell Size: " << total_sell_size << "\n"
                          << "  Net Position: " << sum_of_orders << "\n"
                          << "  Current Trade ID: " << current_trade_.tradeId << std::endl;
                
                // Update current trade size and direction
                current_trade_.size = sum_of_orders;
                current_trade_.is_long = (sum_of_orders >= 0);
                
                // Only consider the trade closed if the net position is effectively zero
                is_trade_closed = std::abs(sum_of_orders) < 1e-8;

                std::cout << "[DEBUG] Trade status after calculation:\n"
                          << "  Is trade closed: " << (is_trade_closed ? "Yes" : "No") << "\n"
                          << "  Current size: " << current_trade_.size << "\n"
                          << "  Is long: " << (current_trade_.is_long ? "Yes" : "No") << std::endl;

                // Update cumulative reward and total size for position reduction
                if (pnl != 0.0 && filled_size > 0 && avg_price > 0) {
                    std::cout << "\033[1;36m[REWARD DEBUG] Starting reward calculation:\033[0m\n"
                              << "  PnL: " << pnl << " USDT\n"
                              << "  Filled Size: " << filled_size << " contracts\n"
                              << "  Avg Price: " << avg_price << " USDT\n"
                              << "  Previous Filled: " << previous_filled << " contracts\n"
                              << "  Current Cumulative Reward: " << current_trade_.cumulative_reward << "\n"
                              << "  Current Total Size: " << current_trade_.total_size << std::endl;

                    double pnl_percentage = pnl / (filled_size * avg_price);
                    if (std::isfinite(pnl_percentage)) {
                        // Only add the new fill amount to total_size
                        double previous_filled = 0.0;
                        for (const auto& order : current_trade_.orders) {
                            if (order.okx_order_id == okx_order_id) {
                                previous_filled = order.filled_size;
                                break;
                            }
                        }
                        double new_fill_amount = filled_size - previous_filled;
                        double reward_increment = new_fill_amount * pnl_percentage;
                        
                        std::cout << "\033[1;36m[REWARD DEBUG] Calculation details:\033[0m\n"
                                  << "  PnL Percentage: " << (pnl_percentage * 100.0) << "%\n"
                                  << "  New Fill Amount: " << new_fill_amount << " contracts\n"
                                  << "  Reward Increment: " << reward_increment << "\n"
                                  << "  MaxDD: " << okx_ws_->get_maxdd() << std::endl;

                        current_trade_.cumulative_reward += reward_increment;
                        current_trade_.total_size += new_fill_amount;

                        std::cout << "\033[1;36m[REWARD DEBUG] Updated values:\033[0m\n"
                                  << "  New Cumulative Reward: " << current_trade_.cumulative_reward << "\n"
                                  << "  New Total Size: " << current_trade_.total_size << std::endl;
                    } else {
                        std::cout << "\033[1;31m[REWARD DEBUG] Warning: Invalid PnL percentage calculation\033[0m\n"
                                  << "  PnL: " << pnl << "\n"
                                  << "  Filled Size: " << filled_size << "\n"
                                  << "  Avg Price: " << avg_price << std::endl;
                    }
                }

                // Update cumulative prices and sizes based on order side
                std::string current_side;
                for (const auto& current_order : current_trade_.orders) {
                    if (current_order.okx_order_id == okx_order_id) {
                        current_side = current_order.side;
                        if (current_order.side == "buy") {
                            current_trade_.buy_side_cumulative_price += filled_size * avg_price;
                            current_trade_.buy_side_total_size += filled_size;
                            
                            std::cout << "\033[1;36m[PRICE DEBUG] Updated buy side averages:\033[0m\n"
                                      << "  New fill: " << filled_size << " @ " << avg_price << "\n"
                                      << "  Cumulative price sum: " << current_trade_.buy_side_cumulative_price << "\n"
                                      << "  Total buy size: " << current_trade_.buy_side_total_size << "\n"
                                      << "  Average buy price: " << current_trade_.get_avg_buy_price() << std::endl;
                        } else {
                            current_trade_.sell_side_cumulative_price += filled_size * avg_price;
                            current_trade_.sell_side_total_size += filled_size;
                            
                            std::cout << "\033[1;36m[PRICE DEBUG] Updated sell side averages:\033[0m\n"
                                      << "  New fill: " << filled_size << " @ " << avg_price << "\n"
                                      << "  Cumulative price sum: " << current_trade_.sell_side_cumulative_price << "\n"
                                      << "  Total sell size: " << current_trade_.sell_side_total_size << "\n"
                                      << "  Average sell price: " << current_trade_.get_avg_sell_price() << std::endl;
                        }
                        break;
                    }
                }

                // Check if trade should be closed due to size reaching 0
                if (std::abs(current_trade_.size) < 1e-8) {
                    is_trade_closed = true;
                    
                    // Calculate final reward using new formula
                    double final_reward = 0.0;
                    double avg_buy_price = current_trade_.get_avg_buy_price();
                    double avg_sell_price = current_trade_.get_avg_sell_price();
                    
                    std::cout << "\033[1;36m[REWARD DEBUG] Calculating final reward for closed trade:\033[0m\n"
                              << "  Average Buy Price: " << avg_buy_price << "\n"
                              << "  Average Sell Price: " << avg_sell_price << "\n"
                              << "  Trade Direction: " << (current_trade_.is_long ? "LONG" : "SHORT") << "\n"
                              << "  MaxDD: " << okx_ws_->get_maxdd() << std::endl;
                    
                    if (avg_buy_price > 0 && avg_sell_price > 0) {
                        if (current_trade_.is_long) {
                            // For long trades: ((sellprice-buyprice) / buyprice) * 100 * 100
                            final_reward = ((avg_sell_price - avg_buy_price) / avg_buy_price) * 100.0 * 100.0;
                        } else {
                            // For short trades: ((buyprice-sellprice) / sellprice) * 100 * 100
                            final_reward = ((avg_buy_price - avg_sell_price) / avg_sell_price) * 100.0 * 100.0;
                        }
                        
                        std::cout << "\033[1;36m[REWARD DEBUG] Base reward calculated:\033[0m\n"
                                  << "  Base Reward: " << final_reward << "%" << std::endl;
                        
                        // Apply MaxDD adjustment
                        if (final_reward > 0) {
                            final_reward *= (1.0 - 2.0 * std::abs(okx_ws_->get_maxdd()));
                            std::cout << "\033[1;36m[REWARD DEBUG] Positive reward adjusted for MaxDD:\033[0m\n"
                                      << "  MaxDD Multiplier: " << (1.0 - 2.0 * std::abs(okx_ws_->get_maxdd())) << "\n"
                                      << "  Final Reward: " << final_reward << "%" << std::endl;
                        } else if (final_reward < 0) {
                            final_reward *= (1.0 + 2.0 * std::abs(okx_ws_->get_maxdd()));
                            std::cout << "\033[1;36m[REWARD DEBUG] Negative reward adjusted for MaxDD:\033[0m\n"
                                      << "  MaxDD Multiplier: " << (1.0 + 2.0 * std::abs(okx_ws_->get_maxdd())) << "\n"
                                      << "  Final Reward: " << final_reward << "%" << std::endl;
                        }
                    }
                    
                    std::cout << "\033[1;36m[REWARD DEBUG] Trade closure summary:\033[0m\n"
                              << "  Initial Size: " << previous_size << "\n"
                              << "  Total Buy Size: " << current_trade_.buy_side_total_size << "\n"
                              << "  Total Sell Size: " << current_trade_.sell_side_total_size << "\n"
                              << "  Average Buy Price: " << avg_buy_price << "\n"
                              << "  Average Sell Price: " << avg_sell_price << "\n"
                              << "  Final Reward: " << final_reward << "%\n"
                              << "  Maximum Drawdown: " << okx_ws_->get_maxdd() << std::endl;
                    
                    // First ensure the closing order is in current_trade_.orders
                    bool closing_order_found = false;
                    for (const auto& order : current_trade_.orders) {
                        if (order.okx_order_id == okx_order_id) {
                            closing_order_found = true;
                            break;
                        }
                    }
                    
                    if (!closing_order_found) {
                        // Calculate closing and opening sizes based on the position analysis that was already done
                        double closing_size = std::min(fill_delta, std::abs(previous_size));
                        double opening_size = fill_delta - closing_size;

                        // First handle the closing portion
                        OrderInfo closing_order;
                        closing_order.state_id = state_id;
                        closing_order.okx_order_id = okx_order_id;
                        closing_order.filled_size = closing_size;  // Only use closing size here
                        closing_order.avg_fill_price = avg_price;
                        closing_order.is_filled = (state == "filled");
                        closing_order.has_okx_id = true;
                        closing_order.order_state = state;
                        closing_order.volume = intended_volume;
                        closing_order.price = intended_price;
                        closing_order.side = side;
                        closing_order.tradeId = current_trade_.tradeId;
                        
                        // Calculate execution percentage for closing portion
                        closing_order.execution_percentage = (closing_order.volume > 0) ? 
                            (closing_size / closing_order.volume) : 0.0;  // Store as decimal (0-1)
                        
                        // Add fill portion for closing
                        if (closing_size >= 0.001) {
                            OrderInfo::FillPortion closing_portion;
                            closing_portion.tradeId = current_trade_.tradeId;
                            closing_portion.size = closing_size;
                            closing_portion.price = avg_price;
                            closing_portion.timestamp = fill_time;
                            closing_portion.is_closing = true;
                            closing_portion.execution_percentage = closing_order.execution_percentage;  // Keep as decimal
                            closing_order.fill_portions.push_back(closing_portion);
                        }
                        
                        // Add closing order to current trade
                        current_trade_.orders.push_back(closing_order);

                        // Publish execution update for the closing order
                        publishTradeUpdate(state_id, okx_order_id, closing_order.execution_percentage);

                        // Now handle the opening portion if it exists
                        if (opening_size >= 0.001) {
                            // Create a new trade for the opening portion
                            Trade new_trade;
                            new_trade.has_active_trade = true;
                            new_trade.is_long = (side == "buy");
                            new_trade.size = opening_size;
                            new_trade.tradeId = okx_order_id;  // Use order ID as new trade ID
                            
                            // Create opening order
                            OrderInfo opening_order;
                            opening_order.state_id = state_id;
                            opening_order.okx_order_id = okx_order_id;
                            opening_order.filled_size = opening_size;
                            opening_order.avg_fill_price = avg_price;
                            opening_order.is_filled = (state == "filled");
                            opening_order.has_okx_id = true;
                            opening_order.order_state = state;
                            opening_order.volume = intended_volume;
                            opening_order.price = intended_price;
                            opening_order.side = side;
                            opening_order.tradeId = new_trade.tradeId;
                            
                            // Calculate execution percentage for opening portion
                            opening_order.execution_percentage = (opening_order.volume > 0) ? 
                                (opening_size / opening_order.volume) : 0.0;  // Store as decimal (0-1)
                            
                            // Add fill portion for opening
                            OrderInfo::FillPortion opening_portion;
                            opening_portion.tradeId = new_trade.tradeId;
                            opening_portion.size = opening_size;
                            opening_portion.price = avg_price;
                            opening_portion.timestamp = fill_time;
                            opening_portion.is_closing = false;
                            opening_portion.execution_percentage = (opening_order.volume > 0) ? 
                                (opening_size / opening_order.volume) : 0.0;  // Store as decimal (0-1)
                            opening_order.fill_portions.push_back(opening_portion);
                            
                            // Add opening order to new trade
                            new_trade.orders.push_back(opening_order);
                            
                            // Store new trade info for later use
                            next_trade_ = new_trade;
                            has_next_trade_ = true;

                            // Publish execution update for the opening portion
                            publishTradeUpdate(state_id, okx_order_id, opening_order.execution_percentage);
                        }
                        
                        std::cout << "[DEBUG] Processed dual-purpose order:\n"
                                  << "  Order ID: " << okx_order_id 
                                  << ", Total Size: " << filled_size 
                                  << ", Side: " << side << "\n"
                                  << "  Closing Portion: " << closing_size 
                                  << " (Trade ID: " << current_trade_.tradeId 
                                  << ", Execution: " << std::fixed << std::setprecision(2) 
                                  << (closing_size / intended_volume * 100.0) << "%)\n";  // Convert to percentage only for display
                        
                        if (opening_size >= 0.001) {
                            std::cout << "  Opening Portion: " << opening_size 
                                      << " (Trade ID: " << okx_order_id 
                                      << ", Execution: " << std::fixed << std::setprecision(2)
                                      << (opening_size / intended_volume * 100.0) << "%)\n"  // Convert to percentage only for display
                                      << "  New Trade Created: Yes (Size: " << opening_size << ")" << std::endl;
                        }
                    }
                    
                    // Prepare filled portions for trade closure message
                    std::vector<std::pair<std::string, double>> filled_portions;
                    for (const auto& order : current_trade_.orders) {
                        // For dual-purpose orders, add separate entries for each portion
                        if (!order.fill_portions.empty()) {
                            for (const auto& portion : order.fill_portions) {
                                // Convert decimal to percentage (0-100) for output
                                double exec_pct = std::min(100.0, portion.execution_percentage * 100.0);
                                filled_portions.push_back(std::make_pair(
                                    order.okx_order_id,
                                    exec_pct
                                ));
                            }
                        } else {
                            // Convert decimal to percentage (0-100) for output
                            double exec_pct = std::min(100.0, order.execution_percentage * 100.0);
                            filled_portions.push_back(std::make_pair(
                                order.okx_order_id,
                                exec_pct
                            ));
                        }
                    }

                    // Publish trade closure update
                    publishTradeUpdate(state_id, okx_order_id, true, filled_portions);
                    
                    printTradeOrders();
                    
                    // Store current trade ID before clearing
                    std::string prev_trade_id = current_trade_.tradeId;
                    
                    // If we have a next trade ready, switch to it
                    if (has_next_trade_) {
                        current_trade_ = next_trade_;
                        has_next_trade_ = false;
                    } else {
                        // Otherwise clear the trade struct
                        current_trade_ = Trade();
                    }
                    
                    std::cout << "[DEBUG] Trade transition completed:\n"
                              << "  Previous Trade ID: " << prev_trade_id << "\n"
                              << "  New Trade ID: " << current_trade_.tradeId << "\n"
                              << "  New Position Size: " << current_trade_.size << std::endl;
                    
                    need_balance_update = true;
                    return;  // Exit immediately after trade closure
                } else {
                    // For position reduction without closure, publish after updating trade struct
                    if (filled_size > 0) {  // Changed from closing_size to filled_size
                        publishTradeUpdate(state_id, okx_order_id);
                    }
                }
            }
        }

        // Update order state in deque
        {
            std::lock_guard<std::mutex> lock(okx_ws_->orders_mutex_);
            auto it = okx_ws_->orders_.begin();
            while (it != okx_ws_->orders_.end()) {
                if (it->okx_order_id == okx_order_id) {
                    it->filled_size = filled_size;
                    it->avg_fill_price = avg_price;
                    it->order_state = state;
                    
                    // Update execution percentage based on state first
                    if (state == "filled") {
                        it->execution_percentage = 1.0;  // If order is filled, it's 100%
                        it->is_filled = true;
                    } else {
                        // Only update execution percentage if not filled
                        it->execution_percentage = (it->volume > 0) ? (filled_size / it->volume) : 0.0;
                        it->is_filled = false;
                    }
                    
                    // Remove the order if it's fully filled
                    if (state == "filled" || it->execution_percentage >= 1.0) {
                        // Ensure the order is in known_orders_ before removing from deque
                        known_orders_[it->okx_order_id] = it->state_id;
                        it = okx_ws_->orders_.erase(it);
                    } else {
                        ++it;
                    }
                } else {
                    ++it;
                }
            }

            // Limit deque size to 300 by removing oldest orders if necessary
            while (okx_ws_->orders_.size() > 300) {
                // Ensure the order is in known_orders_ before removing from deque
                if (!okx_ws_->orders_.front().okx_order_id.empty()) {
                    known_orders_[okx_ws_->orders_.front().okx_order_id] = okx_ws_->orders_.front().state_id;
                }
                okx_ws_->orders_.pop_front();  // Remove oldest order
            }
        }

        printTradeOrders();

        if (need_balance_update) {
            std::cout << "[" << getCurrentTimestamp() << "] Waiting for balance update from WebSocket..." << std::endl;
        }

        std::cout << "[DEBUG] ========== Order Fill Callback End ==========\n" << std::endl;
    });
}

OMSHandler::~OMSHandler() {
    stop();
}

bool OMSHandler::initializeOKXWebSocket() {
    if (!okx_ws_->connect()) {
        throw std::runtime_error("Failed to initialize OKX WebSocket connection");
    }

    if (!okx_ws_->is_balance_received()) {
        throw std::runtime_error("Failed to receive balance from OKX");
    }

    return true;
}

void OMSHandler::start() {
    if (is_running_) {
        return;
    }

    try {
        // First initialize OKX WebSocket and get initial balance
        if (!initializeOKXWebSocket()) {
            throw std::runtime_error("Failed to initialize OKX WebSocket connection");
        }
        
        // Then initialize RabbitMQ
        initializeRabbitMQ();
        
        // Declare exchanges and queues
        declareExchangesAndQueues();
        
        is_running_ = true;

        // Start consuming messages
        amqp_basic_consume(conn_, 1,
            amqp_cstring_bytes("oms_action_queue"),
            amqp_empty_bytes, 0, 1, 0,
            amqp_empty_table);

        std::cout << "OMS service started. Listening for PPO actions..." << std::endl;

        // Message consumption loop
        while (is_running_) {
            amqp_envelope_t envelope;
            amqp_maybe_release_buffers(conn_);
            
            struct timeval timeout;
            timeout.tv_sec = 1;
            timeout.tv_usec = 0;
            
            auto result = amqp_consume_message(conn_, &envelope, &timeout, 0);
            
            if (result.reply_type == AMQP_RESPONSE_NORMAL) {
                std::string message(static_cast<char*>(envelope.message.body.bytes),
                                  envelope.message.body.len);
                
                try {
                    handleMessage(message);
                } catch (const std::exception& e) {
                    std::cerr << "Error processing message: " << e.what() << std::endl;
                }
                
                amqp_destroy_envelope(&envelope);
            }
        }

    } catch (const std::exception& e) {
        std::cerr << "Error in OMS service: " << e.what() << std::endl;
        stop();
        throw;
    }
}

void OMSHandler::stop() {
    if (!is_running_) return;
    
    is_running_ = false;
    cleanupRabbitMQ();
    
    if (okx_ws_) {
        okx_ws_->disconnect();
    }
}

void OMSHandler::initializeRabbitMQ() {
    conn_ = amqp_new_connection();
    socket_ = amqp_tcp_socket_new(conn_);
    if (!socket_) {
        throw std::runtime_error("Creating TCP socket failed");
    }

    int status = amqp_socket_open(socket_, host_.c_str(), port_);
    if (status != AMQP_STATUS_OK) {
        throw std::runtime_error("Opening TCP socket failed");
    }

    auto login_status = amqp_login(conn_, "/", 0, 131072, 0, AMQP_SASL_METHOD_PLAIN,
                                 username_.c_str(), password_.c_str());
    if (login_status.reply_type != AMQP_RESPONSE_NORMAL) {
        throw std::runtime_error("RabbitMQ login failed");
    }

    amqp_channel_open(conn_, 1);
    auto channel_status = amqp_get_rpc_reply(conn_);
    if (channel_status.reply_type != AMQP_RESPONSE_NORMAL) {
        throw std::runtime_error("Opening channel failed");
    }
}

void OMSHandler::cleanupRabbitMQ() {
    if (conn_) {
        try {
            amqp_channel_close(conn_, 1, AMQP_REPLY_SUCCESS);
            amqp_connection_close(conn_, AMQP_REPLY_SUCCESS);
            amqp_destroy_connection(conn_);
        } catch (...) {
            // Ignore cleanup errors
        }
        conn_ = nullptr;
        socket_ = nullptr;
    }
}

void OMSHandler::declareExchangesAndQueues() {
    // Declare oms exchange for binary messages
    amqp_exchange_declare(conn_, 1,
        amqp_cstring_bytes("oms"), amqp_cstring_bytes("topic"),
        0, 1, 0, 0, amqp_empty_table);

    // Declare execution exchange for execution updates
    amqp_exchange_declare(conn_, 1,
        amqp_cstring_bytes("execution-exchange"), amqp_cstring_bytes("topic"),
        0, 1, 0, 0, amqp_empty_table);

    // Declare and bind queue for PPO actions
    amqp_queue_declare(conn_, 1,
        amqp_cstring_bytes("oms_action_queue"),
        0, 1, 0, 0,
        amqp_empty_table);
    
    amqp_queue_bind(conn_, 1,
        amqp_cstring_bytes("oms_action_queue"),
        amqp_cstring_bytes("oms"),
        amqp_cstring_bytes("oms.action"),
        amqp_empty_table);
}

bool OMSHandler::place_order(uint32_t state_id, const std::string& inst_id,
                           const std::string& td_mode, const std::string& side,
                           const std::string& ord_type, double size, double price,
                           double original_volume, double original_price) {
    
    // Validate and adjust order size
    std::vector<OrderInfo> orders_vec(okx_ws_->orders_.begin(), okx_ws_->orders_.end());
    auto size_result = pos_size_handler_->validateAndAdjustSize(
        size,
        side,
        okx_ws_->get_balance(),
        current_trade_,
        orders_vec,
        price  // Use the order price as mid_price for margin calculations
    );

    if (!size_result.can_place_order) {
        std::cout << "[" << getCurrentTimestamp() << "] Order rejected: " 
                  << size_result.reason << "\n" 
                  << size_result.calculation_log;
        return false;
    }

    // Use adjusted size if necessary
    double final_size = size_result.adjusted_size;
    if (size_result.was_adjusted) {
        std::cout << "[" << getCurrentTimestamp() << "] Order size adjusted from " 
                  << size << " to " << final_size << "\n" 
                  << size_result.calculation_log;
    }

    // Place order with validated/adjusted size
    return okx_ws_->send_order(state_id, inst_id, td_mode, side, 
                              ord_type, final_size, price,
                              original_volume, original_price);
}

void OMSHandler::handleMessage(const std::string& message) {
    try {
        // Expected size for V2 format: 23 bytes
        // (1 byte action type + 8 bytes price + 8 bytes volume + 4 bytes mid-price + 2 bytes state ID)
        if (message.size() != 23) {
            throw std::runtime_error("Invalid message size for V2 format");
        }

        uint8_t action_type;
        double price, volume, mid_price;
        uint16_t state_id;
        binary_utils::decodeOmsActionV2(message.data(), action_type, price, volume, mid_price, state_id);

        std::cout << "[" << getCurrentTimestamp() << "] Received action: "
                  << "Type=" << static_cast<int>(action_type)
                  << " Price=" << price
                  << " Volume=" << volume
                  << " MidPrice=" << mid_price
                  << " StateID=" << state_id << std::endl;

        // Process the action based on mid-price
        processAction(action_type, price, volume, mid_price, state_id);

    } catch (const std::exception& e) {
        std::cerr << "Error processing binary message: " << e.what() << std::endl;
    }
}

void OMSHandler::processAction(uint8_t action_type, double price, double volume, double mid_price, uint32_t state_id) {
    try {
        // Calculate trading parameters
        constexpr double LEVERAGE = 100.0;
        constexpr double MIN_CONTRACT_SIZE = 0.1;  // Minimum contract size
        const double balance = okx_ws_->get_balance();
        
        // Calculate order parameters
        const double order_price = mid_price * (1.0 + (price / 1000.0));
        const std::string side = price < 0 ? "buy" : "sell";
        const std::string order_type = action_type == 0 ? "limit" : "market";
        const double margin = balance * 0.001 * volume;
        double size = LEVERAGE * margin / (order_price / 100.0);
        
        // Round size to one decimal place (ceiling)
        size = std::ceil(size * 10.0) / 10.0;
        
        // Ignore orders with size less than minimum contract size
        if (size < MIN_CONTRACT_SIZE) {
            std::cout << "Calculated size " << size << " is below minimum. Ignoring order." << std::endl;
            return;
        }

        // Place the order using cross mode instead of isolated
        place_order(state_id, "BTC-USDT-SWAP", "cross", side, order_type, size, order_price, volume, price);

        // Log the calculated parameters
        std::cout << "[" << getCurrentTimestamp() << "] Trading Parameters:\n"
                  << "  Side: " << side << "\n"
                  << "  Order Type: " << order_type << "\n"
                  << "  Mid Price: " << std::fixed << std::setprecision(2) << mid_price << " USD\n"
                  << "  Order Price: " << order_price << " USD\n"
                  << "  Balance: " << balance << " USDT\n"
                  << "  Margin: " << margin << " USDT\n"
                  << "  Leverage: " << LEVERAGE << "x\n"
                  << "  Size: " << size << " contracts" << std::endl;

    } catch (const std::exception& e) {
        std::cerr << "Error processing action: " << e.what() << std::endl;
    }
}

void OMSHandler::printTradeOrders() const {
    std::cout << "\n============== Trade Orders ==============\n";
    std::cout << "Active: " << (current_trade_.has_active_trade ? "Yes" : "No") << "\n";
    std::cout << "Direction: " << (current_trade_.is_long ? "LONG" : "SHORT") << "\n";
    std::cout << std::fixed << std::setprecision(8);  // Set fixed precision for all numeric output
    std::cout << "Current Size: " << current_trade_.size << " contracts\n";
    std::cout << "Total Reduced Size: " << current_trade_.total_size << " contracts\n";
    std::cout << "Cumulative Reward: " << current_trade_.cumulative_reward << "\n";
    std::cout << "Trade ID: " << current_trade_.tradeId << "\n";
    
    // Only calculate and print average reward if total_size is non-zero
    if (current_trade_.total_size > 0) {
        double avg_reward = current_trade_.cumulative_reward / current_trade_.total_size;
        std::cout << "Average Reward: " << avg_reward << "\n";
    }
    
    std::cout << "Orders:\n";
    std::cout << "  State ID    Filled Size      Avg Price             OKX Order ID      Side     Executed %    Status                Trade ID" << std::endl;
    std::cout << "=============================================================================================================================" << std::endl;
    
    for (const auto& order : current_trade_.orders) {
        std::cout << std::setw(10) << order.state_id
                  << std::setw(14) << order.filled_size
                  << std::setw(14) << order.avg_fill_price
                  << std::setw(25) << order.okx_order_id
                  << std::setw(10) << order.side
                  << std::setw(12) << (order.execution_percentage * 100.0) << "%"  // Convert to percentage for display
                  << std::setw(20) << order.order_state
                  << std::setw(25) << order.tradeId << std::endl;
        
        if (!order.fill_portions.empty()) {
            std::cout << "    Fill Portions:" << std::endl;
            for (const auto& portion : order.fill_portions) {
                std::cout << "      Trade ID: " << portion.tradeId
                          << ", Size: " << std::fixed << std::setprecision(8) << portion.size
                          << ", Price: " << std::fixed << std::setprecision(8) << portion.price << std::endl;
            }
        }
    }
    std::cout << "=============================================================================================================================" << std::endl;
    std::cout << "=========================================\n\n";
}

std::string OMSHandler::getCurrentTimestamp() const {
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

void OMSHandler::publishTradeUpdate(uint32_t state_id, const std::string& okx_id) {
    // Skip if we've already published this state_id
    if (published_state_ids_.find(state_id) != published_state_ids_.end()) {
        return;
    }

    // Find the order in current trade to check execution
    bool has_execution = false;
    for (const auto& order : current_trade_.orders) {
        if (order.okx_order_id == okx_id && order.filled_size > 0) {
            has_execution = true;
            break;
        }
    }

    // Skip if order wasn't executed
    if (!has_execution) {
        return;
    }

    try {
        // Create JSON message for single execution
        nlohmann::json update;
        update["state_id"] = state_id;
        update["okx_id"] = okx_id;
        update["is_trade_closed"] = false;  // Single execution is not a trade closure

        std::string message = update.dump();

        // Publish to RabbitMQ
        amqp_basic_properties_t props;
        props._flags = AMQP_BASIC_CONTENT_TYPE_FLAG | AMQP_BASIC_DELIVERY_MODE_FLAG;
        props.content_type = amqp_cstring_bytes("application/json");
        props.delivery_mode = 2; // persistent delivery mode

        int status = amqp_basic_publish(conn_,
                                      1,
                                      amqp_cstring_bytes("execution-exchange"),
                                      amqp_cstring_bytes("execution.update"),
                                      0,
                                      0,
                                      &props,
                                      amqp_cstring_bytes(message.c_str()));

        if (status != AMQP_STATUS_OK) {
            std::cerr << "Failed to publish execution update" << std::endl;
        } else {
            // Mark this state_id as published
            published_state_ids_.insert(state_id);
        }

    } catch (const std::exception& e) {
        std::cerr << "Error publishing execution update: " << e.what() << std::endl;
    }
}

void OMSHandler::publishTradeUpdate(uint32_t state_id, 
                                  const std::string& okx_id,
                                  bool is_trade_closed,
                                  const std::vector<std::pair<std::string, double>>& filled_portions) {
    if (!is_trade_closed && filled_portions.empty()) {
        return;  // Only skip for non-closure updates if no executions occurred
    }

    try {
        nlohmann::json update;
        update["is_trade_closed"] = is_trade_closed;
        
        // Add filled portions array, including all orders from this trade
        nlohmann::json portions = nlohmann::json::array();
        
        // For trade closure, include ALL orders that were part of this trade
        for (const auto& order : current_trade_.orders) {
            if (order.tradeId == current_trade_.tradeId) {
                if (is_trade_closed) {
                    // For trade closure, include every order with its execution percentage
                nlohmann::json portion_obj;
                    portion_obj[order.okx_order_id] = order.execution_percentage;
                portions.push_back(portion_obj);
                } else {
                    // For regular updates, only include orders with opening portions
                    double total_opening_size = 0.0;
                    for (const auto& portion : order.fill_portions) {
                        if (!portion.is_closing) {
                            total_opening_size += portion.size;
                        }
                    }
                    if (total_opening_size > 0) {
                        nlohmann::json portion_obj;
                        portion_obj[order.okx_order_id] = total_opening_size / order.volume;
                        portions.push_back(portion_obj);
                    }
                }
            }
        }
        
        update["filled_portions"] = portions;

        // Add reward for trade closure messages
        if (is_trade_closed) {
            double avg_buy_price = current_trade_.get_avg_buy_price();
            double avg_sell_price = current_trade_.get_avg_sell_price();
            
            if (avg_buy_price > 0 && avg_sell_price > 0) {
                double final_reward = 0.0;
                if (current_trade_.is_long) {
                    final_reward = ((avg_sell_price - avg_buy_price) / avg_buy_price) * 100.0;
                } else {
                    final_reward = ((avg_buy_price - avg_sell_price) / avg_sell_price) * 100.0;
                }
                
                // Apply MaxDD adjustment
                if (final_reward > 0) {
                    final_reward *= (1.0 - 2.0 * std::abs(okx_ws_->get_maxdd()));
                } else if (final_reward < 0) {
                    final_reward *= (1.0 + 2.0 * std::abs(okx_ws_->get_maxdd()));
                }
                
            update["reward"] = final_reward;
            }
        }

        std::string message = update.dump();

        // Publish to RabbitMQ
        amqp_basic_properties_t props;
        props._flags = AMQP_BASIC_CONTENT_TYPE_FLAG | AMQP_BASIC_DELIVERY_MODE_FLAG;
        props.content_type = amqp_cstring_bytes("application/json");
        props.delivery_mode = 2; // persistent delivery mode

        int status = amqp_basic_publish(conn_,
                                      1,
                                      amqp_cstring_bytes("execution-exchange"),
                                      amqp_cstring_bytes("execution.update"),
                                      0,
                                      0,
                                      &props,
                                      amqp_cstring_bytes(message.c_str()));

        if (status != AMQP_STATUS_OK) {
            std::cerr << "Failed to publish trade closure update" << std::endl;
        }

    } catch (const std::exception& e) {
        std::cerr << "Error publishing trade closure update: " << e.what() << std::endl;
    }
}

void OMSHandler::publishTradeUpdate(uint32_t state_id, 
                                  const std::string& okx_id,
                                  double execution_percentage) {
    // Skip if we've already published this state_id
    if (published_state_ids_.find(state_id) != published_state_ids_.end()) {
        return;
    }

    // Skip if no execution occurred
    if (execution_percentage <= 0.0) {
        return;
    }

    try {
        // Create JSON message for execution update
        nlohmann::json update;
        update["state_id"] = state_id;
        update["okx_id"] = okx_id;
        update["is_trade_closed"] = false;
        update["execution_percentage"] = execution_percentage;

        std::string message = update.dump();

        // Publish to RabbitMQ
        amqp_basic_properties_t props;
        props._flags = AMQP_BASIC_CONTENT_TYPE_FLAG | AMQP_BASIC_DELIVERY_MODE_FLAG;
        props.content_type = amqp_cstring_bytes("application/json");
        props.delivery_mode = 2; // persistent delivery mode

        int status = amqp_basic_publish(conn_,
                                      1,
                                      amqp_cstring_bytes("execution-exchange"),
                                      amqp_cstring_bytes("execution.update"),
                                      0,
                                      0,
                                      &props,
                                      amqp_cstring_bytes(message.c_str()));

        if (status != AMQP_STATUS_OK) {
            std::cerr << "Failed to publish execution update" << std::endl;
        } else {
            // Mark this state_id as published
            published_state_ids_.insert(state_id);
        }

    } catch (const std::exception& e) {
        std::cerr << "Error publishing execution update: " << e.what() << std::endl;
    }
} 