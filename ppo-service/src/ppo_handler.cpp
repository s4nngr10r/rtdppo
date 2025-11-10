#include "../include/ppo_handler.hpp"
#include <binary_utils.hpp>
#include <iostream>
#include <iomanip>
#include <stdexcept>
#include <thread>
#include <chrono>
#include <sstream>
#include <filesystem>

PPOHandler::PPOHandler(const std::string& host, int port,
                     const std::string& username, const std::string& password)
    : host_(host), port_(port), username_(username), password_(password),
      is_running_(false), conn_(nullptr), socket_(nullptr),
      actor_(INPUT_SIZE), critic_(INPUT_SIZE), state_counter_(0) {
    
    // Initialize random seed
    srand(static_cast<unsigned int>(time(nullptr)));
    
    // Create models directory if it doesn't exist
    std::filesystem::create_directories(MODEL_DIR);
    
    initializeNetworks();
    
    // Try to load existing model
    if (loadModel()) {
        std::cout << "Loaded pre-trained model successfully" << std::endl;
    } else {
        std::cout << "Starting with fresh model" << std::endl;
    }
}

PPOHandler::~PPOHandler() {
    // Save model on shutdown
    try {
        saveModel("shutdown");
    } catch (const std::exception& e) {
        std::cerr << "Error saving model during shutdown: " << e.what() << std::endl;
    }
    stop();
}

void PPOHandler::initializeNetworks() {
    try {
        // Initialize networks
        actor_->to(torch::kFloat64);
        critic_->to(torch::kFloat64);

        // Initialize optimizers with double precision
        actor_optimizer_ = std::make_unique<torch::optim::Adam>(
            actor_->parameters(), torch::optim::AdamOptions(learning_rate_));
        critic_optimizer_ = std::make_unique<torch::optim::Adam>(
            critic_->parameters(), torch::optim::AdamOptions(learning_rate_));

        actor_->eval();
        critic_->eval();

        std::cout << "Neural networks initialized with double precision" << std::endl;
    } catch (const c10::Error& e) {
        std::cerr << "LibTorch error during network initialization: " << e.what() << std::endl;
        throw;
    } catch (const std::exception& e) {
        std::cerr << "Error during network initialization: " << e.what() << std::endl;
        throw;
    }
}

void PPOHandler::declareExchangesAndQueues() {
    // Declare oms exchange for binary messages
    amqp_exchange_declare(conn_, 1,
        amqp_cstring_bytes("oms"), amqp_cstring_bytes("topic"),
        0, 1, 0, 0, amqp_empty_table);

    // Declare execution exchange for execution updates
    amqp_exchange_declare(conn_, 1,
        amqp_cstring_bytes("execution-exchange"), amqp_cstring_bytes("topic"),
        0, 1, 0, 0, amqp_empty_table);

    // Declare and bind queue for orderbook updates
    amqp_queue_declare(conn_, 1,
        amqp_cstring_bytes("ppo_queue"),
        0, 1, 0, 0,
        amqp_empty_table);
    
    amqp_queue_bind(conn_, 1,
        amqp_cstring_bytes("ppo_queue"),
        amqp_cstring_bytes("orderbook"),
        amqp_cstring_bytes("orderbook.updates"),
        amqp_empty_table);

    // Declare and bind queue for execution updates
    amqp_queue_declare(conn_, 1,
        amqp_cstring_bytes("ppo_execution_queue"),
        0, 1, 0, 0,
        amqp_empty_table);
    
    amqp_queue_bind(conn_, 1,
        amqp_cstring_bytes("ppo_execution_queue"),
        amqp_cstring_bytes("execution-exchange"),
        amqp_cstring_bytes("execution.update"),
        amqp_empty_table);
}

void PPOHandler::start() {
    if (is_running_) {
        return;
    }

    try {
        initializeRabbitMQ();
        declareExchangesAndQueues();
        is_running_ = true;

        // Start consuming messages from both queues with manual ack
        amqp_basic_consume(conn_, 1,
            amqp_cstring_bytes("ppo_queue"),
            amqp_empty_bytes, 0, 0, 0,  // Changed auto_ack to 0
            amqp_empty_table);

        amqp_basic_consume(conn_, 1,
            amqp_cstring_bytes("ppo_execution_queue"),
            amqp_empty_bytes, 0, 0, 0,  // Changed auto_ack to 0
            amqp_empty_table);

        std::cout << "PPO service started. Listening for orderbook and execution updates..." << std::endl;

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
                    // Check the routing key to determine message type
                    std::string routing_key(static_cast<char*>(envelope.routing_key.bytes),
                                          envelope.routing_key.len);
                    
                    if (routing_key == "orderbook.updates") {
                        handleMessage(message);
                        // Acknowledge message after successful processing
                        amqp_basic_ack(conn_, 1, envelope.delivery_tag, 0);
                    } else if (routing_key == "execution.update") {
                        handleExecutionUpdate(message);
                        // Acknowledge message after successful processing
                        amqp_basic_ack(conn_, 1, envelope.delivery_tag, 0);
                    }
                } catch (const std::exception& e) {
                    std::cerr << "Error processing message: " << e.what() << std::endl;
                    // Reject and requeue message on error
                    amqp_basic_reject(conn_, 1, envelope.delivery_tag, 1);
                }
                
                amqp_destroy_envelope(&envelope);
            }
        }

    } catch (const std::exception& e) {
        std::cerr << "Error in PPO service: " << e.what() << std::endl;
        stop();
        throw;
    }
}

void PPOHandler::stop() {
    is_running_ = false;
    cleanupRabbitMQ();
}

void PPOHandler::initializeRabbitMQ() {
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

void PPOHandler::cleanupRabbitMQ() {
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

void PPOHandler::handleMessage(const std::string& message) {
    try {
        const size_t expected_size = (OrderBookState::LEVELS * 2 * OrderBookState::VALUES_PER_LEVEL + 
                                    1 + OrderBookState::NUM_DEPTHS * OrderBookState::NUM_FEATURES) * sizeof(uint64_t) +
                                    sizeof(uint32_t) +  // 4 bytes for mid-price
                                    sizeof(uint16_t);   // 2 bytes for state ID

        if (message.size() != expected_size) {
            throw std::runtime_error("Invalid message size: got " + std::to_string(message.size()) + 
                                   " bytes, expected " + std::to_string(expected_size) + " bytes");
        }

        const uint64_t* data = reinterpret_cast<const uint64_t*>(message.data());
        OrderBookState state;
        size_t offset = 0;
        
        // Process bids
        for (size_t i = 0; i < OrderBookState::LEVELS; ++i) {
            double* level = state.bid_level(i);
            level[0] = binary_utils::decodeChangeValue(data[offset++]);    // price
            level[1] = binary_utils::decodeOrderBookValue(data[offset++]); // volume
            level[2] = binary_utils::decodeOrderBookValue(data[offset++]); // orders
        }
        
        // Process asks
        for (size_t i = 0; i < OrderBookState::LEVELS; ++i) {
            double* level = state.ask_level(i);
            level[0] = binary_utils::decodeChangeValue(data[offset++]);    // price
            level[1] = binary_utils::decodeOrderBookValue(data[offset++]); // volume
            level[2] = binary_utils::decodeOrderBookValue(data[offset++]); // orders
        }
        
        // Process mid price change
        state.mid_price_change = binary_utils::decodeChangeValue(data[offset++]);
        
        // Process market features
        for (size_t depth = 0; depth < OrderBookState::NUM_DEPTHS; ++depth) {
            state.feature_at(depth, 0) = binary_utils::decodeChangeValue(data[offset++]); // volume imbalance
            state.feature_at(depth, 1) = binary_utils::decodeChangeValue(data[offset++]); // order imbalance
            state.feature_at(depth, 2) = binary_utils::decodeChangeValue(data[offset++]); // bid VWAP change
            state.feature_at(depth, 3) = binary_utils::decodeChangeValue(data[offset++]); // ask VWAP change
        }

        // Get mid-price from the last 4 bytes before state ID
        const uint32_t* mid_price_cents = reinterpret_cast<const uint32_t*>(
            message.data() + message.size() - sizeof(uint16_t) - sizeof(uint32_t));
        state.mid_price = static_cast<double>(*mid_price_cents) / binary_utils::CENTS_MULTIPLIER;

        // Get state ID from the last two bytes
        const uint16_t* last_bytes = reinterpret_cast<const uint16_t*>(
            message.data() + message.size() - sizeof(uint16_t));
        state.state_id = *last_bytes;

        // Maintain the 1000-state history buffer
        if (state_buffer_.size() >= HISTORY_BUFFER_SIZE) {
            state_buffer_.pop_front();
        }
        state_buffer_.push_back(std::move(state));
        
        // Increment state counter and save if needed
        state_counter_++;
        if (state_counter_ % SAVE_INTERVAL == 0) {
            try {
                saveModel();
                std::cout << "[" << getCurrentTimestamp() << "] Model saved after " 
                          << state_counter_ << " states" << std::endl;
            } catch (const std::exception& e) {
                std::cerr << "Error saving model at interval: " << e.what() << std::endl;
            }
        }

        // Process if we have enough states for network input
        if (state_buffer_.size() >= NETWORK_INPUT_SIZE && state_buffer_.back().state_id % 2 == 0) {
            trigger_state_id_ = state_buffer_.back().state_id;
            forwardPass();
        }

    } catch (const std::exception& e) {
        std::cerr << "Error processing binary message: " << e.what() << std::endl;
    }
}

torch::Tensor PPOHandler::preprocessState() {
    std::vector<double> features;
    features.reserve(OrderBookState::TOTAL_FEATURES * NETWORK_INPUT_SIZE);
    
    // Use only the newest NETWORK_INPUT_SIZE states
    auto start_it = std::prev(state_buffer_.end(), NETWORK_INPUT_SIZE);
    auto end_it = state_buffer_.end();
    
    for (auto it = start_it; it != end_it; ++it) {
        // Direct copy, no type conversion
        features.insert(features.end(), it->bids.begin(), it->bids.end());
        features.insert(features.end(), it->asks.begin(), it->asks.end());
        features.push_back(it->mid_price_change);
        features.insert(features.end(), it->features.begin(), it->features.end());
    }
    
    // Create float64 tensor with shape [1, 80, 2421]
    return torch::from_blob(
        features.data(),
        {1, NETWORK_INPUT_SIZE, OrderBookState::TOTAL_FEATURES},
        torch::TensorOptions().dtype(torch::kFloat64)
    ).clone();
}

void PPOHandler::forwardPass() {
    torch::NoGradGuard no_grad;
    
    auto input_tensor = preprocessState();
    
    // Forward pass through actor network (all in float64)
    auto [price_tensor, volume_tensor] = actor_->forward(input_tensor);
    
    // Get the values from tensors
    double price_value = price_tensor.item<double>();
    double volume_value = volume_tensor.item<double>();

    // Apply exploration during initial period (first 1000 states)
    constexpr size_t EXPLORATION_PERIOD = 1000;
    if (state_counter_ < EXPLORATION_PERIOD) {
        // During exploration, randomly flip the price signal with 50% probability
        if (rand() % 2 == 0) {
            price_value = -price_value;
            std::cout << "[" << getCurrentTimestamp() << "] Exploration: Flipped price signal to " 
                      << price_value << " (State " << state_counter_ << "/" << EXPLORATION_PERIOD << ")" << std::endl;
        }
    }

    // Create new tensors with possibly modified values
    auto modified_price_tensor = torch::tensor(price_value, torch::TensorOptions().dtype(torch::kFloat64));
    auto modified_volume_tensor = volume_tensor;
    
    // Publish the action with possibly modified values
    publishAction(modified_price_tensor, modified_volume_tensor);
}

void PPOHandler::publishAction(const torch::Tensor& price, const torch::Tensor& volume) {
    try {
        // Get the values from tensors
        double price_value = price.item<double>();
        double volume_value = volume.item<double>();
        
        // Store action in the buffer with the newest state ID
        ActionInfo action;
        action.price = price_value;
        action.volume = volume_value;
        action.state_id = state_buffer_.back().state_id;  // Use the newest state ID
        
        // Maintain action buffer size
        if (action_buffer_.size() >= ACTION_BUFFER_SIZE) {
            action_buffer_.pop_front();
        }
        action_buffer_.push_back(action);

        // 23 bytes: 1 byte action type + 8 bytes price + 8 bytes volume + 4 bytes mid-price + 2 bytes state ID
        std::vector<char> buffer(23);
        
        // Get current mid-price from the latest state
        double current_mid_price = state_buffer_.back().mid_price;
        uint16_t current_state_id = state_buffer_.back().state_id;
        
        // Encode action, price, volume, mid-price, and state ID using encodeOmsActionV2
        binary_utils::encodeOmsActionV2(buffer.data(), 0, price_value, volume_value, current_mid_price, current_state_id);
        
        // Publish to RabbitMQ
        amqp_basic_properties_t props;
        props._flags = AMQP_BASIC_CONTENT_TYPE_FLAG | AMQP_BASIC_DELIVERY_MODE_FLAG;
        props.content_type = amqp_cstring_bytes("application/octet-stream");
        props.delivery_mode = 2; // persistent delivery mode

        // Publish the message using direct RabbitMQ connection
        int status = amqp_basic_publish(
            conn_,
            1,
            amqp_cstring_bytes("oms"),
            amqp_cstring_bytes("oms.action"),
            0,
            0,
            &props,
            amqp_bytes_t{buffer.size(), buffer.data()}
        );

        if (status != AMQP_STATUS_OK) {
            throw std::runtime_error("Failed to publish action");
        }

        // Log the action storage
        std::cout << "[" << getCurrentTimestamp() << "] Stored action: "
                  << "Price=" << price_value
                  << " Volume=" << volume_value
                  << " MidPrice=" << current_mid_price
                  << " StateID=" << current_state_id
                  << " (Buffer size: " << action_buffer_.size() << ")" << std::endl;

    } catch (const std::exception& e) {
        std::cerr << "Error publishing action: " << e.what() << std::endl;
    }
}

void PPOHandler::handleExecutionUpdate(const std::string& message) {
    try {
        auto json = nlohmann::json::parse(message);
        std::cout << "[" << getCurrentTimestamp() << "] Execution Update:\n" 
                  << json.dump(2) << std::endl;

        bool is_trade_closed = json["is_trade_closed"].get<bool>();

        if (is_trade_closed) {
            // Check if we have any orders before processing closure
            if (current_trade_.orders.empty() && !json.contains("filled_portions")) {
                std::cerr << "Error: Received trade closure without any orders" << std::endl;
                return;
            }

            // Process filled portions if available
            if (json.contains("filled_portions") && !json["filled_portions"].empty()) {
                for (const auto& portion : json["filled_portions"]) {
                    for (auto& [okx_id, execution_percentage] : portion.items()) {
                        // Find the order in our current trade
                        auto order_it = std::find_if(current_trade_.orders.begin(), current_trade_.orders.end(),
                            [&okx_id](const OrderInfo& order) { return order.okx_id == okx_id; });
                        
                        if (order_it != current_trade_.orders.end()) {
                            order_it->coefficient = execution_percentage;
                        }
                    }
                }
            }

            // Set trade reward if available
            if (json.contains("reward")) {
                current_trade_.reward = json["reward"].get<double>();
            }

            // Log the completed trade
            std::cout << "[" << getCurrentTimestamp() << "] Trade closed:\n"
                      << "  Reward: " << current_trade_.reward << "\n"
                      << "  Orders: " << current_trade_.orders.size() << "\n";
            for (const auto& order : current_trade_.orders) {
                std::cout << "    OKX ID: " << order.okx_id 
                          << ", Coefficient: " << order.coefficient 
                          << ", States: " << order.state_ids.size()
                          << ", Action found: " << (order.action.state_id != 0) << "\n";
            }

            // Update networks with completed trade before resetting
            if (!current_trade_.orders.empty()) {
                std::cout << "[" << getCurrentTimestamp() << "] Starting network update..." << std::endl;
                updateNetworks(current_trade_);
                std::cout << "[" << getCurrentTimestamp() << "] Network update completed" << std::endl;
            }

            // Reset trade for next one
            current_trade_ = TradeInfo();
        } else {
            // For non-closure updates, we still need state_id and okx_id
            if (!json.contains("state_id") || !json.contains("okx_id")) {
                std::cerr << "Error: Missing required fields in execution update" << std::endl;
                return;
            }

            uint32_t state_id = json["state_id"].get<uint32_t>();
            std::string okx_id = json["okx_id"].get<std::string>();

            // Check if we already have this order
            auto existing_order = std::find_if(current_trade_.orders.begin(), current_trade_.orders.end(),
                [&okx_id](const OrderInfo& order) { return order.okx_id == okx_id; });
            
            if (existing_order != current_trade_.orders.end()) {
                std::cout << "Warning: Duplicate order update received for OKX ID: " << okx_id << std::endl;
                return;
            }

            // Handle single order update
            OrderInfo order;
            
            // Handle state ID wraparound and range calculation
            std::vector<uint16_t> state_sequence;
            uint16_t start_state_id;
            
            if (state_id >= NETWORK_INPUT_SIZE) {
                start_state_id = state_id - NETWORK_INPUT_SIZE + 1;
            } else {
                // Handle wraparound
                uint16_t wraparound_start = std::numeric_limits<uint16_t>::max() - 
                    (NETWORK_INPUT_SIZE - state_id - 1);
                start_state_id = wraparound_start;
            }

            // Collect state sequence
            bool has_all_states = true;
            for (uint16_t i = 0; i < NETWORK_INPUT_SIZE; ++i) {
                uint16_t current_state = (start_state_id + i) % std::numeric_limits<uint16_t>::max();
                state_sequence.push_back(current_state);
            }

            order.state_ids = state_sequence;

            // Find matching action in buffer
            std::vector<ActionInfo> matching_actions;
            for (const auto& action : action_buffer_) {
                if (action.state_id == state_id) {
                    matching_actions.push_back(action);
                }
            }

            if (matching_actions.empty()) {
                std::cout << "Warning: No matching action found for state ID: " << state_id << std::endl;
            } else if (matching_actions.size() > 1) {
                std::cout << "Warning: Multiple actions found for state ID: " << state_id << std::endl;
            } else {
                order.action = matching_actions[0];
            }

            order.okx_id = okx_id;
            order.coefficient = 0.0;  // Default coefficient
            
            // Add to current trade
            current_trade_.orders.push_back(order);

            std::cout << "[" << getCurrentTimestamp() << "] Added order to trade:\n"
                      << "  OKX ID: " << order.okx_id 
                      << ", States: " << order.state_ids.size()
                      << ", Action found: " << (order.action.state_id != 0)
                      << ", Complete state sequence: " << has_all_states << "\n";
        }

    } catch (const std::exception& e) {
        std::cerr << "Error parsing execution update: " << e.what() << std::endl;
    }
}

std::string PPOHandler::getCurrentTimestamp() const {
    auto now = std::chrono::system_clock::now();
    auto now_time = std::chrono::system_clock::to_time_t(now);
    std::stringstream ss;
    ss << std::put_time(std::localtime(&now_time), "%Y-%m-%d %H:%M:%S");
    return ss.str();
}

void PPOHandler::updateNetworks(const TradeInfo& completed_trade) {
    // Store trade in training buffer
    training_buffer_.push_back(completed_trade);
    if (training_buffer_.size() > MAX_TRAINING_BUFFER_SIZE) {
        training_buffer_.erase(training_buffer_.begin());
    }

    // Enter training mode
    actor_->train();
    critic_->train();

    try {
        // Get states and compute values
        auto states = getStatesFromTrade(completed_trade);
        auto values = getValuesFromStates(states);
        
        // Get action probabilities that were used during execution
        auto [old_price_probs, old_volume_probs] = getProbabilitiesFromStates(states);
        
        // Extract coefficients
        std::vector<double> coefficients;
        coefficients.reserve(completed_trade.orders.size());
        for (const auto& order : completed_trade.orders) {
            coefficients.push_back(order.coefficient);
        }
        
        // Compute advantages using coefficients
        auto advantages = computeAdvantages(states, values, coefficients, completed_trade.reward);
        
        // PPO training loop
        for (int epoch = 0; epoch < ppo_epochs_; ++epoch) {
            // Get current probabilities
            auto [new_price_probs, new_volume_probs] = getProbabilitiesFromStates(states);
            
            // Compute returns for value function
            auto returns = advantages + torch::stack(values);
            
            // Compute losses
            auto policy_loss = computePPOLoss(advantages, old_price_probs, old_volume_probs,
                                            new_price_probs, new_volume_probs, coefficients);
            auto value_loss = computeValueLoss(torch::stack(values), returns, coefficients);
            auto entropy_loss = computeEntropyLoss(new_price_probs, new_volume_probs, coefficients);
            
            // Total loss
            auto total_loss = policy_loss + value_coef_ * value_loss - entropy_coef_ * entropy_loss;
            
            // Optimize
            actor_optimizer_->zero_grad();
            critic_optimizer_->zero_grad();
            total_loss.backward();
            actor_optimizer_->step();
            critic_optimizer_->step();
        }
    } catch (const std::exception& e) {
        std::cerr << "Error during network update: " << e.what() << std::endl;
    }

    // Return to eval mode
    actor_->eval();
    critic_->eval();
}

torch::Tensor PPOHandler::computeAdvantages(
    const std::vector<torch::Tensor>& states,
    const std::vector<torch::Tensor>& values,
    const std::vector<double>& coefficients,
    double reward) {
    
    auto device = states[0].device();
    auto advantages = torch::zeros({static_cast<long>(states.size())}, 
                                 torch::TensorOptions().dtype(torch::kFloat64).device(device));
    
    // Compute weighted advantages
    for (size_t i = 0; i < states.size(); ++i) {
        advantages[i] = reward * coefficients[i] - values[i].item<double>();
    }
    
    return advantages;
}

torch::Tensor PPOHandler::computePPOLoss(
    const torch::Tensor& advantages,
    const torch::Tensor& old_price_probs,
    const torch::Tensor& old_volume_probs,
    const torch::Tensor& new_price_probs,
    const torch::Tensor& new_volume_probs,
    const std::vector<double>& coefficients) {
    
    auto device = advantages.device();
    auto coeff_tensor = torch::tensor(coefficients, 
                                    torch::TensorOptions().dtype(torch::kFloat64).device(device));
    
    // Compute probability ratios
    auto price_ratio = new_price_probs / old_price_probs;
    auto volume_ratio = new_volume_probs / old_volume_probs;
    
    // Compute clipped ratios
    auto price_clipped = torch::clamp(price_ratio, 1.0 - clip_epsilon_, 1.0 + clip_epsilon_);
    auto volume_clipped = torch::clamp(volume_ratio, 1.0 - clip_epsilon_, 1.0 + clip_epsilon_);
    
    // Compute losses with advantage weighting
    auto price_loss = -torch::min(price_ratio * advantages, price_clipped * advantages);
    auto volume_loss = -torch::min(volume_ratio * advantages, volume_clipped * advantages);
    
    // Apply coefficients and mean
    return ((price_loss + volume_loss) * coeff_tensor.unsqueeze(1)).mean();
}

torch::Tensor PPOHandler::computeValueLoss(
    const torch::Tensor& values,
    const torch::Tensor& returns,
    const std::vector<double>& coefficients) {
    
    auto device = values.device();
    auto coeff_tensor = torch::tensor(coefficients, 
                                    torch::TensorOptions().dtype(torch::kFloat64).device(device));
    
    // MSE loss weighted by coefficients
    auto value_loss = torch::pow(values - returns, 2);
    return (value_loss * coeff_tensor.unsqueeze(1)).mean();
}

torch::Tensor PPOHandler::computeEntropyLoss(
    const torch::Tensor& price_probs,
    const torch::Tensor& volume_probs,
    const std::vector<double>& coefficients) {
    
    auto device = price_probs.device();
    auto coeff_tensor = torch::tensor(coefficients, 
                                    torch::TensorOptions().dtype(torch::kFloat64).device(device));
    
    // Compute entropy for both heads
    auto price_entropy = -(price_probs * torch::log(price_probs + 1e-10));
    auto volume_entropy = -(volume_probs * torch::log(volume_probs + 1e-10));
    
    // Weight by coefficients and mean
    return ((price_entropy + volume_entropy) * coeff_tensor.unsqueeze(1)).mean();
}

std::vector<torch::Tensor> PPOHandler::getStatesFromTrade(const TradeInfo& trade) {
    std::vector<torch::Tensor> states;
    states.reserve(trade.orders.size());
    
    for (const auto& order : trade.orders) {
        // Reconstruct state tensor from state IDs
        std::vector<double> features;
        features.reserve(OrderBookState::TOTAL_FEATURES * NETWORK_INPUT_SIZE);
        
        // Find states in buffer by IDs
        for (auto state_id : order.state_ids) {
            auto state_it = std::find_if(state_buffer_.begin(), state_buffer_.end(),
                [state_id](const OrderBookState& state) { return state.state_id == state_id; });
            
            if (state_it != state_buffer_.end()) {
                features.insert(features.end(), state_it->bids.begin(), state_it->bids.end());
                features.insert(features.end(), state_it->asks.begin(), state_it->asks.end());
                features.push_back(state_it->mid_price_change);
                features.insert(features.end(), state_it->features.begin(), state_it->features.end());
            }
        }
        
        // Create tensor
        if (features.size() == OrderBookState::TOTAL_FEATURES * NETWORK_INPUT_SIZE) {
            states.push_back(torch::from_blob(
                features.data(),
                {1, NETWORK_INPUT_SIZE, OrderBookState::TOTAL_FEATURES},
                torch::TensorOptions().dtype(torch::kFloat64)
            ).clone());
        }
    }
    
    return states;
}

std::tuple<torch::Tensor, torch::Tensor> PPOHandler::getActionsFromTrade(const TradeInfo& trade) {
    std::vector<double> prices, volumes;
    prices.reserve(trade.orders.size());
    volumes.reserve(trade.orders.size());
    
    for (const auto& order : trade.orders) {
        prices.push_back(order.action.price);
        volumes.push_back(order.action.volume);
    }
    
    auto options = torch::TensorOptions().dtype(torch::kFloat64);
    return std::make_tuple(
        torch::tensor(prices, options),
        torch::tensor(volumes, options)
    );
}

std::vector<torch::Tensor> PPOHandler::getValuesFromStates(const std::vector<torch::Tensor>& states) {
    std::vector<torch::Tensor> values;
    values.reserve(states.size());
    
    for (const auto& state : states) {
        values.push_back(critic_->forward(state));
    }
    
    return values;
}

std::tuple<torch::Tensor, torch::Tensor> PPOHandler::getProbabilitiesFromStates(
    const std::vector<torch::Tensor>& states) {
    
    std::vector<torch::Tensor> price_probs, volume_probs;
    price_probs.reserve(states.size());
    volume_probs.reserve(states.size());
    
    for (const auto& state : states) {
        auto [price, volume] = actor_->forward(state);
        price_probs.push_back(price);
        volume_probs.push_back(volume);
    }
    
    return std::make_tuple(
        torch::stack(price_probs),
        torch::stack(volume_probs)
    );
}

std::string PPOHandler::getModelPath() const {
    return MODEL_DIR + "/ppo_model.pt";
}

void PPOHandler::saveModel(const std::string& reason) {
    try {
        // Create a serialization archive
        torch::serialize::OutputArchive archive;
        
        // Save actor state
        archive.write("actor", actor_->parameters());
        
        // Save critic state
        archive.write("critic", critic_->parameters());
        
        // Save optimizer states
        std::vector<torch::Tensor> actor_optim_params;
        std::vector<torch::Tensor> critic_optim_params;
        
        for (const auto& param_group : actor_optimizer_->param_groups()) {
            for (const auto& p : param_group.params()) {
                if (p.grad().defined()) {
                    actor_optim_params.push_back(p.grad().clone());
                }
            }
        }
        
        for (const auto& param_group : critic_optimizer_->param_groups()) {
            for (const auto& p : param_group.params()) {
                if (p.grad().defined()) {
                    critic_optim_params.push_back(p.grad().clone());
                }
            }
        }
        
        archive.write("actor_optim", actor_optim_params);
        archive.write("critic_optim", critic_optim_params);
        
        // Save to file
        archive.save_to(getModelPath());
        
        std::cout << "[" << getCurrentTimestamp() << "] Model saved (" << reason << ")" << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "Error saving model: " << e.what() << std::endl;
        throw;
    }
}

bool PPOHandler::loadModel() {
    try {
        if (!std::filesystem::exists(getModelPath())) {
            return false;
        }
        
        // Load archive
        torch::serialize::InputArchive archive;
        archive.load_from(getModelPath());
        
        // Create temporary IValues for reading
        c10::IValue actor_ivalue;
        c10::IValue critic_ivalue;
        c10::IValue actor_optim_ivalue;
        c10::IValue critic_optim_ivalue;
        
        // Read values into IValues
        archive.read("actor", actor_ivalue);
        archive.read("critic", critic_ivalue);
        archive.read("actor_optim", actor_optim_ivalue);
        archive.read("critic_optim", critic_optim_ivalue);
        
        // Convert IValues to tensor vectors
        std::vector<torch::Tensor> actor_params = actor_ivalue.toTensorVector();
        std::vector<torch::Tensor> critic_params = critic_ivalue.toTensorVector();
        std::vector<torch::Tensor> actor_optim_params = actor_optim_ivalue.toTensorVector();
        std::vector<torch::Tensor> critic_optim_params = critic_optim_ivalue.toTensorVector();
        
        // Apply parameters to networks
        auto actor_params_it = actor_params.begin();
        for (auto& param : actor_->parameters()) {
            if (actor_params_it != actor_params.end()) {
                param.data().copy_(*actor_params_it++);
            }
        }
        
        auto critic_params_it = critic_params.begin();
        for (auto& param : critic_->parameters()) {
            if (critic_params_it != critic_params.end()) {
                param.data().copy_(*critic_params_it++);
            }
        }
        
        // Apply gradients to optimizers
        auto actor_optim_it = actor_optim_params.begin();
        for (auto& param_group : actor_optimizer_->param_groups()) {
            for (auto& p : param_group.params()) {
                if (actor_optim_it != actor_optim_params.end()) {
                    if (!p.grad().defined()) {
                        p.mutable_grad() = torch::zeros_like(p);
                    }
                    p.mutable_grad().copy_(*actor_optim_it++);
                }
            }
        }
        
        auto critic_optim_it = critic_optim_params.begin();
        for (auto& param_group : critic_optimizer_->param_groups()) {
            for (auto& p : param_group.params()) {
                if (critic_optim_it != critic_optim_params.end()) {
                    if (!p.grad().defined()) {
                        p.mutable_grad() = torch::zeros_like(p);
                    }
                    p.mutable_grad().copy_(*critic_optim_it++);
                }
            }
        }
        
        // Ensure models are in eval mode
        actor_->eval();
        critic_->eval();
        
        return true;
    } catch (const std::exception& e) {
        std::cerr << "Error loading model: " << e.what() << std::endl;
        return false;
    }
} 