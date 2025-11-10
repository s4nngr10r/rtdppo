#pragma once
#include <cstdint>
#include <limits>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace binary_utils {

// Precomputed constants for faster encoding/decoding
constexpr uint64_t PRICE_SIGN_MASK = 1ULL << 63;
constexpr uint64_t PRICE_FRAC_MASK = (1ULL << 63) - 1;
constexpr double PRICE_FRAC_SCALE = static_cast<double>((1ULL << 63) - 1);
constexpr double PRICE_FRAC_SCALE_INV = 1.0 / static_cast<double>((1ULL << 63) - 1);

constexpr uint64_t ORDERBOOK_SIGN_MASK = 1ULL << 63;
constexpr uint64_t ORDERBOOK_WHOLE_MASK = ((1ULL << 10) - 1) << 53;
constexpr uint64_t ORDERBOOK_FRAC_MASK = (1ULL << 53) - 1;
constexpr double ORDERBOOK_FRAC_SCALE = static_cast<double>((1ULL << 53) - 1);
constexpr double ORDERBOOK_FRAC_SCALE_INV = 1.0 / static_cast<double>((1ULL << 53) - 1);

// Constants for binary message format
constexpr uint8_t ACTION_TYPE_MASK = 0x07;  // 3 bits for action type
constexpr double ZERO_THRESHOLD = 1e-15;     // Threshold for zero checks
constexpr uint32_t MAX_MIDPRICE = 1000000;   // Maximum mid-price value
constexpr uint32_t CENTS_MULTIPLIER = 100;   // For cent precision

// Helper function for consistent zero checks
inline bool isZero(double value) {
    return std::abs(value) < ZERO_THRESHOLD;
}

// For price changes, VWAP changes, imbalance changes, etc.
// Format: 1 bit sign, 63 bits fraction
inline uint64_t encodeChangeValue(double value) {
    if (isZero(value)) return 0;

    const uint64_t sign = value < 0 ? 1ULL : 0ULL;
    const double absValue = std::abs(value);
    const uint64_t fraction = static_cast<uint64_t>(absValue * PRICE_FRAC_SCALE);
    
    return (sign << 63) | (fraction & PRICE_FRAC_MASK);
}

inline double decodeChangeValue(uint64_t encoded) {
    if (encoded == 0) return 0.0;
    
    const bool is_negative = (encoded & PRICE_SIGN_MASK) != 0;
    const uint64_t fraction = encoded & PRICE_FRAC_MASK;
    const double value = static_cast<double>(fraction) * PRICE_FRAC_SCALE_INV;
    
    return is_negative ? -value : value;
}

// For volume and order changes in orderbook levels
// Format: 1 bit sign, 10 bits whole number, 53 bits fraction
inline uint64_t encodeOrderBookValue(double value) {
    if (isZero(value)) return 0;

    const uint64_t sign = value < 0 ? 1ULL : 0ULL;
    const double absValue = std::abs(value);
    
    double wholePart;
    const double fracPart = std::modf(absValue, &wholePart);
    
    // Ensure whole part fits in 10 bits (0-1023)
    const uint64_t wholeInt = static_cast<uint64_t>(std::min(wholePart, 1023.0));
    const uint64_t fractionInt = static_cast<uint64_t>(fracPart * ORDERBOOK_FRAC_SCALE);
    
    return (sign << 63) | ((wholeInt & ((1ULL << 10) - 1)) << 53) | (fractionInt & ORDERBOOK_FRAC_MASK);
}

inline double decodeOrderBookValue(uint64_t encoded) {
    if (encoded == 0) return 0.0;
    
    const bool is_negative = (encoded & ORDERBOOK_SIGN_MASK) != 0;
    const uint64_t whole = (encoded & ORDERBOOK_WHOLE_MASK) >> 53;
    const uint64_t fraction = encoded & ORDERBOOK_FRAC_MASK;
    
    double value = static_cast<double>(whole) + 
                  static_cast<double>(fraction) * ORDERBOOK_FRAC_SCALE_INV;
    
    return is_negative ? -value : value;
}

// For OMS action messages
// Format: 1 byte action type + 8 bytes price + 8 bytes volume
inline void encodeOmsAction(char* __restrict buffer, uint8_t action_type,
                          double price, double volume) {
    // Write action type (3 bits)
    buffer[0] = action_type & ACTION_TYPE_MASK;

    // Write price using change value encoding
    *reinterpret_cast<uint64_t*>(buffer + 1) = encodeChangeValue(price);

    // Write volume using orderbook value encoding
    *reinterpret_cast<uint64_t*>(buffer + 9) = encodeOrderBookValue(volume);
}

inline void decodeOmsAction(const char* __restrict buffer, uint8_t& __restrict action_type,
                          double& __restrict price, double& __restrict volume) {
    // Read action type (3 bits)
    action_type = buffer[0] & ACTION_TYPE_MASK;

    // Read price using change value decoding
    price = decodeChangeValue(*reinterpret_cast<const uint64_t*>(buffer + 1));

    // Read volume using orderbook value decoding
    volume = decodeOrderBookValue(*reinterpret_cast<const uint64_t*>(buffer + 9));
}

// For OMS action messages with mid-price
// Format: 1 byte action type + 8 bytes price + 8 bytes volume + 4 bytes mid-price + 2 bytes state ID
inline void encodeOmsActionV2(char* __restrict buffer, uint8_t action_type,
                            double price, double volume, double mid_price, uint16_t state_id) {
    // Validate mid-price range
    if (mid_price < 0.0 || mid_price > static_cast<double>(MAX_MIDPRICE)) {
        throw std::runtime_error("Mid-price must be between 0.00 and 1000000.00");
    }

    // Write action type (3 bits)
    buffer[0] = action_type & ACTION_TYPE_MASK;

    // Write price using change value encoding
    *reinterpret_cast<uint64_t*>(buffer + 1) = encodeChangeValue(price);

    // Write volume using orderbook value encoding
    *reinterpret_cast<uint64_t*>(buffer + 9) = encodeOrderBookValue(volume);

    // Write mid-price with cent precision (4 bytes)
    uint32_t mid_price_cents = static_cast<uint32_t>(std::round(mid_price * CENTS_MULTIPLIER));
    *reinterpret_cast<uint32_t*>(buffer + 17) = mid_price_cents;

    // Write state ID (2 bytes)
    *reinterpret_cast<uint16_t*>(buffer + 21) = state_id;
}

inline void decodeOmsActionV2(const char* __restrict buffer, uint8_t& __restrict action_type,
                            double& __restrict price, double& __restrict volume,
                            double& __restrict mid_price, uint16_t& __restrict state_id) {
    // Read action type (3 bits)
    action_type = buffer[0] & ACTION_TYPE_MASK;

    // Read price using change value decoding
    price = decodeChangeValue(*reinterpret_cast<const uint64_t*>(buffer + 1));

    // Read volume using orderbook value decoding
    volume = decodeOrderBookValue(*reinterpret_cast<const uint64_t*>(buffer + 9));

    // Read mid-price and convert back from cents to dollars
    uint32_t mid_price_cents = *reinterpret_cast<const uint32_t*>(buffer + 17);
    mid_price = static_cast<double>(mid_price_cents) / CENTS_MULTIPLIER;

    // Read state ID
    state_id = *reinterpret_cast<const uint16_t*>(buffer + 21);
}

// Decode state ID from message
inline uint16_t decodeStateId(const char* data) {
    return *reinterpret_cast<const uint16_t*>(data);
}

} // namespace binary_utils 