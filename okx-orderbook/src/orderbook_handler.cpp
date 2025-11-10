#include "../include/orderbook_handler.hpp"
#include <binary_utils.hpp>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <chrono>
#include <simdjson.h>

void OrderBookHandler::handleMessage(const std::string& message) {
    auto start_time = std::chrono::high_resolution_clock::now();
    
    try {
        // Parse JSON using simdjson
        json_buffer_ = simdjson::padded_string(message);
        auto doc = parser_.iterate(json_buffer_);
        
        // Handle ping-pong messages
        if (auto op = doc["op"].get_string(); !op.error()) {
            if (op.value() == "ping" || op.value() == "pong") {
                return;
            }
        }
        
        // Handle subscription confirmation and errors
        if (auto event = doc["event"].get_string(); !event.error()) {
            if (event.value() == "error") {
                if (auto msg = doc["msg"].get_string(); !msg.error() && msg.value().find("ping") != std::string_view::npos) {
                    return;
                }
                std::cerr << "WebSocket error: " << doc["msg"].get_string().value() << std::endl;
            } else {
                std::cout << "Event: " << event.value() << std::endl;
            }
            return;
        }

        // Handle data messages
        auto action = doc["action"].get_string();
        auto data = doc["data"].get_array();
        if (!action.error() && !data.error()) {
            simdjson::ondemand::value first_item;
            auto error = data.at(0).get(first_item);
            if (!error) {
                if (action.value() == "snapshot") {
                    handleSnapshot(std::move(first_item));
                    publishOrderBookUpdate();
                } else if (action.value() == "update") {
                    processOrderBookUpdate(std::move(first_item));
                    publishOrderBookUpdate();
                }

                auto end_time = std::chrono::high_resolution_clock::now();
                auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time);
                
                logAverageProcessingTime(duration);
            }
        }

    } catch (const simdjson::simdjson_error& e) {
        std::cerr << "Error processing message: " << e.what() << std::endl;
    }
}

void OrderBookHandler::logAverageProcessingTime(std::chrono::microseconds current_duration) {
    processing_times_.push_back(current_duration);
    total_messages_processed_++;
    
    if (processing_times_.size() > TIMING_BUFFER_SIZE) {
        processing_times_.pop_front();
    }
    
    // Log average every TIMING_BUFFER_SIZE messages
    if (total_messages_processed_ % TIMING_BUFFER_SIZE == 0 && processing_times_.size() == TIMING_BUFFER_SIZE) {
        auto total_duration = std::chrono::microseconds(0);
        for (const auto& duration : processing_times_) {
            total_duration += duration;
        }
        
        auto average_duration = total_duration.count() / TIMING_BUFFER_SIZE;
        
        std::cout << "[" << getCurrentTimestamp() << "] Average processing time over last " 
                  << TIMING_BUFFER_SIZE << " messages: " << average_duration 
                  << "µs, Current State ID: " << static_cast<int>(current_state_id_) << std::endl;
    }
}

std::string OrderBookHandler::getCurrentTimestamp() const {
    auto now = std::chrono::system_clock::now();
    auto now_c = std::chrono::system_clock::to_time_t(now);
    auto now_ms = std::chrono::duration_cast<std::chrono::microseconds>(
        now.time_since_epoch()) % 1000000;
    
    std::stringstream ss;
    ss << std::put_time(std::localtime(&now_c), "%Y-%m-%d %H:%M:%S")
       << '.' << std::setfill('0') << std::setw(6) << now_ms.count();
    return ss.str();
}

void OrderBookHandler::subscribe(const std::string& instrument) {
    simdjson::padded_string subscription_json = simdjson::padded_string(R"({
        "op": "subscribe",
        "args": [{
            "channel": "books",
            "instId": ")" + instrument + R"("
        }]
    })");
    
    std::cout << "Subscribing to " << instrument << std::endl;
    
    if (ws_client_) {
        ws_client_->setPendingSubscribeMessage(subscription_json.data());
    }
}

void OrderBookHandler::validateOrderBookState() {
    const size_t REQUIRED_LEVELS = 400;  // OKX provides 400 levels per side
    if (bids.size() != REQUIRED_LEVELS || asks.size() != REQUIRED_LEVELS) {
        std::stringstream error_msg;
        error_msg << "Invalid order book state: Expected " << REQUIRED_LEVELS 
                 << " levels, got " << bids.size() << " bids and " 
                 << asks.size() << " asks";
        throw std::runtime_error(error_msg.str());
    }
}

void OrderBookHandler::processOrderBookUpdate(simdjson::ondemand::value&& data) {
    try {
        // Process asks and bids
        if (auto asks_array = data["asks"].get_array(); !asks_array.error()) {
            for (auto ask : asks_array) {
                if (auto ask_array = ask.get_array(); !ask_array.error()) {
                    updatePriceLevel(asks, std::move(ask_array), false);
                }
            }
            std::sort(asks.begin(), asks.end(), compareAsks);
        }

        if (auto bids_array = data["bids"].get_array(); !bids_array.error()) {
            for (auto bid : bids_array) {
                if (auto bid_array = bid.get_array(); !bid_array.error()) {
                    updatePriceLevel(bids, std::move(bid_array), true);
                }
            }
            std::sort(bids.begin(), bids.end(), compareBids);
        }

        validateOrderBookState();

    } catch (const simdjson::simdjson_error& e) {
        std::cerr << "Error processing order book update: " << e.what() << std::endl;
        throw;
    }
}

void OrderBookHandler::updatePriceLevel(std::vector<OrderBookLevel>& side, simdjson::ondemand::array&& level, bool is_bids) {
    size_t idx = 0;
    double price = 0.0, volume = 0.0, orders = 0.0;
    
    // Extract values from the array
    for (auto value : level) {
        if (idx >= 4) break;
        
        if (auto str_val = value.get_string(); !str_val.error()) {
            switch(idx) {
                case 0: price = fast_stod(str_val.value()); break;
                case 1: volume = fast_stod(str_val.value()); break;
                case 3: orders = fast_stod(str_val.value()); break;
            }
        }
        idx++;
    }
    
    if (idx < 4) {
        std::cerr << "Invalid price level format" << std::endl;
        return;
    }

    // Use custom binary search implementation for better cache locality
    size_t left = 0;
    size_t right = side.size();
    
    while (left < right) {
        size_t mid = (left + right) / 2;
        double mid_price = side[mid].price;
        
        if (is_bids) {
            if (mid_price > price) left = mid + 1;
            else if (mid_price < price) right = mid;
            else {
                // Exact match found
                if (volume <= 0.0) {
                    side.erase(side.begin() + mid);
                } else {
                    side[mid].volume = volume;
                    side[mid].orders = orders;
                }
                return;
            }
        } else {
            if (mid_price < price) left = mid + 1;
            else if (mid_price > price) right = mid;
            else {
                // Exact match found
                if (volume <= 0.0) {
                    side.erase(side.begin() + mid);
                } else {
                    side[mid].volume = volume;
                    side[mid].orders = orders;
                }
                return;
            }
        }
    }

    // If volume is 0, no need to insert
    if (volume <= 0.0) return;

    // Insert new level at the correct position
    side.insert(side.begin() + left, OrderBookLevel{price, volume, orders});
}

// Fast string to double conversion
inline double OrderBookHandler::fast_stod(std::string_view str) {
    const char* p = str.data();
    const char* end = p + str.size();
    bool neg = false;
    if (p < end && *p == '-') {
        neg = true;
        ++p;
    }
    
    double val = 0.0;
    while (p < end && *p >= '0' && *p <= '9') {
        val = val * 10.0 + (*p - '0');
        ++p;
    }
    
    if (p < end && *p == '.') {
        double factor = 0.1;
        ++p;
        while (p < end && *p >= '0' && *p <= '9') {
            val += (*p - '0') * factor;
            factor *= 0.1;
            ++p;
        }
    }
    
    if (p < end && (*p == 'e' || *p == 'E')) {
        ++p;
        bool exp_neg = false;
        if (p < end && *p == '-') {
            exp_neg = true;
            ++p;
        } else if (p < end && *p == '+') {
            ++p;
        }
        
        int exp = 0;
        while (p < end && *p >= '0' && *p <= '9') {
            exp = exp * 10 + (*p - '0');
            ++p;
        }
        
        if (exp_neg) {
            while (exp-- > 0) val *= 0.1;
        } else {
            while (exp-- > 0) val *= 10.0;
        }
    }
    
    return neg ? -val : val;
}

void OrderBookHandler::handleSnapshot(simdjson::ondemand::value&& data) {
    try {
        // Clear existing state
        bids.clear();
        asks.clear();

        // Reserve space for efficiency
        const size_t REQUIRED_LEVELS = 400;
        bids.reserve(REQUIRED_LEVELS);
        asks.reserve(REQUIRED_LEVELS);

        // Process initial bids
        if (auto bids_array = data["bids"].get_array(); !bids_array.error()) {
            for (auto bid : bids_array) {
                if (auto bid_array = bid.get_array(); !bid_array.error()) {
                    size_t idx = 0;
                    double price = 0.0, volume = 0.0, orders = 0.0;
                    
                    for (auto value : bid_array) {
                        if (idx >= 4) break;
                        if (auto str_val = value.get_string(); !str_val.error()) {
                            switch(idx) {
                                case 0: price = fast_stod(str_val.value()); break;
                                case 1: volume = fast_stod(str_val.value()); break;
                                case 3: orders = fast_stod(str_val.value()); break;
                            }
                        }
                        idx++;
                    }
                    
                    if (idx >= 4 && volume > 0.0) {
                        bids.emplace_back(price, volume, orders);
                    }
                }
            }
            std::sort(bids.begin(), bids.end(), compareBids);
        }

        // Process initial asks
        if (auto asks_array = data["asks"].get_array(); !asks_array.error()) {
            for (auto ask : asks_array) {
                if (auto ask_array = ask.get_array(); !ask_array.error()) {
                    size_t idx = 0;
                    double price = 0.0, volume = 0.0, orders = 0.0;
                    
                    for (auto value : ask_array) {
                        if (idx >= 4) break;
                        if (auto str_val = value.get_string(); !str_val.error()) {
                            switch(idx) {
                                case 0: price = fast_stod(str_val.value()); break;
                                case 1: volume = fast_stod(str_val.value()); break;
                                case 3: orders = fast_stod(str_val.value()); break;
                            }
                        }
                        idx++;
                    }
                    
                    if (idx >= 4 && volume > 0.0) {
                        asks.emplace_back(price, volume, orders);
                    }
                }
            }
            std::sort(asks.begin(), asks.end(), compareAsks);
        }

        validateOrderBookState();
        previous_mid_price = calculateMidPrice();

    } catch (const simdjson::simdjson_error& e) {
        std::cerr << "Error processing snapshot: " << e.what() << std::endl;
        throw;
    }
}

double OrderBookHandler::calculateMidPrice() const {
    if (!asks.empty() && !bids.empty()) {
        return (asks[0].price + bids[0].price) / 2.0;
    }
    return 0.0;
}

double OrderBookHandler::calculateVolumeImbalance(size_t depth) const {
    double bidVolume = 0.0;
    double askVolume = 0.0;

    // Sum volumes up to specified depth
    for (size_t i = 0; i < depth && i < bids.size(); ++i) {
        bidVolume += bids[i].volume;
    }
    for (size_t i = 0; i < depth && i < asks.size(); ++i) {
        askVolume += asks[i].volume;
    }

    double totalVolume = bidVolume + askVolume;
    if (totalVolume > 0.0) {
        return (bidVolume - askVolume) / totalVolume;  // Range: [-1, 1]
    }
    return 0.0;
}

double OrderBookHandler::calculateOrderImbalance(size_t depth) const {
    double bidOrders = 0.0;
    double askOrders = 0.0;

    // Sum orders up to specified depth
    for (size_t i = 0; i < depth && i < bids.size(); ++i) {
        bidOrders += bids[i].orders;
    }
    for (size_t i = 0; i < depth && i < asks.size(); ++i) {
        askOrders += asks[i].orders;
    }

    double totalOrders = bidOrders + askOrders;
    if (totalOrders > 0.0) {
        return (bidOrders - askOrders) / totalOrders;  // Range: [-1, 1]
    }
    return 0.0;
}

double OrderBookHandler::calculateVWAP(size_t depth, bool is_bids) const {
    const auto& side = is_bids ? bids : asks;
    double volumeSum = 0.0;
    double weightedPriceSum = 0.0;

    for (size_t i = 0; i < depth && i < side.size(); ++i) {
        volumeSum += side[i].volume;
        weightedPriceSum += side[i].price * side[i].volume;
    }

    if (volumeSum > 0.0) {
        return weightedPriceSum / volumeSum;
    }
    return 0.0;
}

double OrderBookHandler::calculateAverageVolume(
    const std::deque<std::vector<OrderBookLevel>>& history, 
    size_t level_idx) const {
    double sum = 0.0;
    size_t count = 0;
    for (const auto& state : history) {
        if (level_idx < state.size()) {
            sum += state[level_idx].volume;
            count++;
        }
    }
    return count > 0 ? sum / count : 0.0;
}

double OrderBookHandler::calculateAverageOrders(
    const std::deque<std::vector<OrderBookLevel>>& history, 
    size_t level_idx) const {
    double sum = 0.0;
    size_t count = 0;
    for (const auto& state : history) {
        if (level_idx < state.size()) {
            sum += state[level_idx].orders;
            count++;
        }
    }
    return count > 0 ? sum / count : 0.0;
}

std::vector<PreprocessedLevel> OrderBookHandler::preprocessLevels(
    const std::vector<OrderBookLevel>& current,
    const std::vector<OrderBookLevel>& previous,
    const std::deque<std::vector<OrderBookLevel>>& history) const {
    
    std::vector<PreprocessedLevel> result;
    result.reserve(current.size());

    for (size_t i = 0; i < current.size(); ++i) {
        PreprocessedLevel level;

        // Calculate price change from previous state
        if (i < previous.size() && previous[i].price != 0.0) {
            level.price_change = ((current[i].price - previous[i].price) / previous[i].price);
        } else {
            level.price_change = 0.0;
        }

        // Calculate volume change from average
        double avg_volume = calculateAverageVolume(history, i);
        if (avg_volume > 0.0) {
            level.volume_change = ((current[i].volume - avg_volume) / avg_volume);
        } else {
            level.volume_change = 0.0;
        }

        // Calculate orders change from average
        double avg_orders = calculateAverageOrders(history, i);
        if (avg_orders > 0.0) {
            level.orders_change = ((current[i].orders - avg_orders) / avg_orders);
        } else {
            level.orders_change = 0.0;
        }

        result.push_back(level);
    }

    return result;
}

void OrderBookHandler::updateHistory() {
    // Update bid history
    previous_bids.push_back(bids);
    if (previous_bids.size() > HISTORY_SIZE) {
        previous_bids.pop_front();
    }

    // Update ask history
    previous_asks.push_back(asks);
    if (previous_asks.size() > HISTORY_SIZE) {
        previous_asks.pop_front();
    }
}

OrderBookFeatures OrderBookHandler::calculateFeatures() const {
    OrderBookFeatures features;
    features.midPrice = calculateMidPrice();

    // Calculate imbalances and VWAP for different depths
    for (size_t i = 0; i < features.depthLevels.size(); ++i) {
        size_t depth = features.depthLevels[i];
        features.volumeImbalance[i] = calculateVolumeImbalance(depth);
        features.orderImbalance[i] = calculateOrderImbalance(depth);
        
        // Calculate bid and ask VWAP changes relative to mid price
        double bid_vwap = calculateVWAP(depth, true);
        double ask_vwap = calculateVWAP(depth, false);
        if (features.midPrice > 0.0) {
            features.bidVwapChange[i] = (bid_vwap - features.midPrice) / features.midPrice;
            features.askVwapChange[i] = (ask_vwap - features.midPrice) / features.midPrice;
        } else {
            features.bidVwapChange[i] = 0.0;
            features.askVwapChange[i] = 0.0;
        }
    }

    return features;
}

void OrderBookHandler::publishOrderBookUpdate() {
    try {
        // Calculate total size needed for the binary message
        const size_t message_size = (bids.size() * LEVEL_VALUES + asks.size() * LEVEL_VALUES + 1 + 
                                   OrderBookFeatures::NUM_DEPTHS * OrderBookFeatures::NUM_FEATURES) * 
                                   VALUE_SIZE + 
                                   sizeof(uint32_t) +  // 4 bytes for mid-price
                                   STATE_ID_SIZE;      // 2 bytes for state ID

        std::vector<char> buffer(message_size);
        char* data = buffer.data();
        size_t offset = 0;

        // Write bids
        for (const auto& level : bids) {
            *reinterpret_cast<uint64_t*>(data + offset) = binary_utils::encodeChangeValue(level.price);
            offset += VALUE_SIZE;
            *reinterpret_cast<uint64_t*>(data + offset) = binary_utils::encodeOrderBookValue(level.volume);
            offset += VALUE_SIZE;
            *reinterpret_cast<uint64_t*>(data + offset) = binary_utils::encodeOrderBookValue(level.orders);
            offset += VALUE_SIZE;
        }

        // Write asks
        for (const auto& level : asks) {
            *reinterpret_cast<uint64_t*>(data + offset) = binary_utils::encodeChangeValue(level.price);
            offset += VALUE_SIZE;
            *reinterpret_cast<uint64_t*>(data + offset) = binary_utils::encodeOrderBookValue(level.volume);
            offset += VALUE_SIZE;
            *reinterpret_cast<uint64_t*>(data + offset) = binary_utils::encodeOrderBookValue(level.orders);
            offset += VALUE_SIZE;
        }

        // Calculate and write features
        auto features = calculateFeatures();
        
        // Write mid price change
        *reinterpret_cast<uint64_t*>(data + offset) = binary_utils::encodeChangeValue(features.midPrice);
        offset += VALUE_SIZE;

        // Write market features
        for (size_t depth = 0; depth < OrderBookFeatures::NUM_DEPTHS; ++depth) {
            *reinterpret_cast<uint64_t*>(data + offset) = binary_utils::encodeChangeValue(features.volumeImbalance[depth]);
            offset += VALUE_SIZE;
            *reinterpret_cast<uint64_t*>(data + offset) = binary_utils::encodeChangeValue(features.orderImbalance[depth]);
            offset += VALUE_SIZE;
            *reinterpret_cast<uint64_t*>(data + offset) = binary_utils::encodeChangeValue(features.bidVwapChange[depth]);
            offset += VALUE_SIZE;
            *reinterpret_cast<uint64_t*>(data + offset) = binary_utils::encodeChangeValue(features.askVwapChange[depth]);
            offset += VALUE_SIZE;
        }

        // Write actual mid-price in cents (4 bytes)
        uint32_t mid_price_cents = static_cast<uint32_t>(features.midPrice * binary_utils::CENTS_MULTIPLIER);
        *reinterpret_cast<uint32_t*>(data + offset) = mid_price_cents;
        offset += sizeof(uint32_t);

        // Write state ID as the last two bytes
        *reinterpret_cast<uint16_t*>(data + offset) = current_state_id_;
        
        // Increment state ID for next update
        incrementStateId();

        // Publish binary message
        rmq_handler_->publishBinaryMessage("orderbook", "orderbook.updates", 
                                         buffer.data(), buffer.size());

    } catch (const std::exception& e) {
        std::cerr << "[ERROR] Failed to publish orderbook update: " << e.what() << std::endl;
    }
}
