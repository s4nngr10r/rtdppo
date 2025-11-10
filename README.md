# rtdppo

Real-Time Dumb PPO. A high-performance, event-driven cryptocurrency trading system implementing Proximal Policy Optimization (PPO) reinforcement learning for automated trading on the OKX exchange. The system consists of three microservices that work together to collect market data, generate trading decisions using neural networks, and execute orders.

## ⚠️ Project Status

**This project is currently abandoned.** It is provided as-is for educational and research purposes. Use at your own risk.

## Overview

rtdppo is a real-time trading system that:
- Collects real-time orderbook data from OKX exchange
- Processes market microstructure features using a PPO-based neural network
- Generates trading decisions with double-precision (float64) computation
- **Continuously learns and self-improves** - the model trains on live trading data and updates itself as it trades
- Executes trades through OKX's WebSocket API with automatic position management

The system is designed for high-frequency trading scenarios with low-latency message processing, binary message encoding, and thread-safe order management. The neural network continuously adapts to market conditions by learning from each trade execution, making it a self-improving trading system.

## Architecture

The system consists of three microservices connected via RabbitMQ:

```
┌─────────────────┐         ┌──────────────┐         ┌──────────────┐
│  OKX Orderbook  │────────▶│   RabbitMQ   │────────▶│  PPO Service │
│     Service     │         │   Message    │         │              │
│                 │         │    Broker    │         │              │
└─────────────────┘         └──────────────┘         └──────────────┘
                                      │                       │
                                      │                       ▼
                                      │              ┌──────────────┐
                                      └─────────────▶│ OMS Service  │
                                                     │              │
                                                     └──────────────┘
                                                              │
                                                              ▼
                                                     ┌──────────────┐
                                                     │  OKX Exchange│
                                                     │  (WebSocket) │
                                                     └──────────────┘
```

### Data Flow

1. **OKX Orderbook Service**: Connects to OKX WebSocket API, maintains a 400-level orderbook, calculates market microstructure features, and publishes binary-encoded updates (19,370 bytes) to RabbitMQ.

2. **PPO Service**: Consumes orderbook updates, maintains a rolling state buffer (80 states), processes data through double-precision neural networks (Actor-Critic architecture), **continuously trains on live trading data**, and publishes trading actions (18 bytes) to RabbitMQ. The model self-improves by learning from trade executions and reward signals.

3. **OMS Service**: Consumes trading actions, calculates order parameters based on account balance, executes orders via OKX WebSocket API, tracks positions, calculates rewards, and publishes execution updates.

## Services

### 1. OKX Orderbook Service

Real-time orderbook data collection and feature extraction service.

**Key Features:**
- WebSocket connection to OKX with SSL/TLS support
- Maintains 400-level orderbook per side (bids/asks)
- Calculates market microstructure features:
  - Mid price
  - Volume imbalance at multiple depths (10, 20, 50, 100, 400)
  - Order imbalance at multiple depths
  - VWAP changes at multiple depths
- Binary message encoding (19,370 bytes per update)
- State ID tracking for end-to-end correlation
- Automatic reconnection and error handling

**See:** [`okx-orderbook/README.md`](okx-orderbook/README.md) for detailed documentation.

### 2. PPO Service

Neural network-based trading decision service using Proximal Policy Optimization.

**Key Features:**
- Double-precision (float64) neural networks
- Actor-Critic architecture with Conv1D and LSTM layers
- **Continuous online learning** - model trains on live trading data and self-improves as it trades
- Fixed-size rolling state buffer (80 states × 2,421 features)
- Zero-copy tensor creation
- Binary action encoding (18 bytes per action)
- State ID tracking for action-state correspondence
- Periodic model saving and loading (every 9000 states)

**Neural Network Architecture:**
- **Actor Network**: Conv1D → Conv1D → LSTM → FC → Price/Volume heads
- **Critic Network**: Conv1D → Conv1D → LSTM → FC → Value head
- Input: [batch_size, 80, 2421] (sequence length, features)
- Output: Price adjustment (-1 to 1) and Volume (0 to 1)

**See:** [`ppo-service/README.md`](ppo-service/README.md) for detailed documentation.

### 3. OMS Service

Order Management System for trade execution and position tracking.

**Key Features:**
- Real-time WebSocket connection to OKX private API
- Thread-safe order management with mutex protection
- Automatic position tracking (Net mode)
- Reward calculation based on PnL and maximum drawdown
- Execution reporting via RabbitMQ
- Automatic trade closure
- Cross-margin mode with 100x leverage
- Maximum drawdown tracking (atomic)

**See:** [`oms-service/README.md`](oms-service/README.md) for detailed documentation.

## Message Formats

### Orderbook Updates (OKX Orderbook → PPO Service)

- **Exchange**: `orderbook` (durable topic exchange)
- **Routing Key**: `orderbook.updates`
- **Format**: Binary (19,370 bytes)
  - 400 bid levels × 24 bytes = 9,600 bytes
  - 400 ask levels × 24 bytes = 9,600 bytes
  - Market features = 168 bytes
  - State ID = 2 bytes

### Trading Actions (PPO Service → OMS Service)

- **Exchange**: `oms` (durable topic exchange)
- **Routing Key**: `oms.action`
- **Format**: Binary (18 bytes)
  - Action type: 1 byte
  - Price: 8 bytes (relative to current price, -1 to 1)
  - Volume: 8 bytes (position size, 0 to 1)
  - State ID: 2 bytes

### Execution Updates (OMS Service → Consumers)

- **Exchange**: `oms` (durable topic exchange)
- **Routing Key**: `execution.update`
- **Format**: JSON
  - Order execution updates
  - Trade closure updates with reward calculation

## Prerequisites

- **C++17** or higher
- **CMake** 3.10 or higher
- **Docker** and **Docker Compose** (recommended)
- **RabbitMQ** (included in docker-compose)
- **LibTorch 2.1.0** (CPU) with double precision support
- **OKX API Credentials** (for OMS service):
  - API Key
  - Secret Key
  - Passphrase

### System Dependencies

- libwebsockets (with SSL support)
- OpenSSL
- RabbitMQ-C client library
- simdjson (for high-performance JSON parsing)
- Boost (system component)
- nlohmann-json

## Quick Start

### Using Docker Compose (Recommended)

1. **Clone the repository:**
```bash
git clone https://github.com/s4nngr10r/rtdppo.git
cd rtdppo
```

2. **Set up OKX API credentials** (for OMS service):
```bash
export OKX_API_KEY="your-api-key"
export OKX_SECRET_KEY="your-secret-key"
export OKX_PASSPHRASE="your-passphrase"
```

3. **Start all services:**
```bash
docker compose up --build
```

This will start:
- RabbitMQ (with management UI on port 15672)
- OKX Orderbook Service
- PPO Service
- OMS Service

4. **Access RabbitMQ Management UI:**
   - URL: http://localhost:15672
   - Username: `guest`
   - Password: `guest`

### Manual Build

1. **Install dependencies:**

**Ubuntu/Debian:**
```bash
sudo apt-get update
sudo apt-get install -y \
    cmake \
    g++ \
    librabbitmq-dev \
    nlohmann-json3-dev \
    libwebsockets-dev \
    libssl-dev \
    libboost-system-dev \
    libsimdjson-dev
```

**macOS:**
```bash
brew install cmake rabbitmq-c nlohmann-json libwebsockets openssl boost simdjson
```

2. **Install LibTorch 2.1.0:**
   - Download from [PyTorch website](https://pytorch.org/get-started/locally/)
   - Extract and set `CMAKE_PREFIX_PATH` to LibTorch directory

3. **Build each service:**
```bash
# Build OKX Orderbook Service
cd okx-orderbook
mkdir build && cd build
cmake ..
make

# Build PPO Service
cd ../../ppo-service
mkdir build && cd build
cmake -DCMAKE_PREFIX_PATH=/path/to/libtorch ..
make

# Build OMS Service
cd ../../oms-service
mkdir build && cd build
cmake ..
make
```

4. **Start RabbitMQ:**
```bash
docker run -d --name rabbitmq \
    -p 5672:5672 \
    -p 15672:15672 \
    rabbitmq:3-management
```

5. **Run services** (in separate terminals):
```bash
# Terminal 1: OKX Orderbook Service
cd okx-orderbook/build
./okx_orderbook

# Terminal 2: PPO Service
cd ppo-service/build
./ppo_service

# Terminal 3: OMS Service
cd oms-service/build
export OKX_API_KEY="your-key"
export OKX_SECRET_KEY="your-secret"
export OKX_PASSPHRASE="your-passphrase"
./oms_service
```

## Configuration

### Environment Variables

All services support RabbitMQ configuration via environment variables:

- `RABBITMQ_HOST`: RabbitMQ server host (default: `localhost`)
- `RABBITMQ_PORT`: RabbitMQ server port (default: `5672`)
- `RABBITMQ_USERNAME`: RabbitMQ username (default: `guest`)
- `RABBITMQ_PASSWORD`: RabbitMQ password (default: `guest`)

**OMS Service** additionally requires:
- `OKX_API_KEY`: OKX API key (required)
- `OKX_SECRET_KEY`: OKX secret key (required)
- `OKX_PASSPHRASE`: OKX passphrase (required)

### Trading Parameters

The OMS service operates with the following fixed parameters:
- **Trading Pair**: BTC-USDT-SWAP
- **Leverage**: 100x
- **Position Mode**: Net
- **Margin Mode**: Cross-margin
- **Minimum Contract Size**: 0.1

## Project Structure

```
rtdppo/
├── common/                 # Shared utilities
│   └── binary_utils.hpp   # Binary encoding/decoding utilities
├── okx-orderbook/         # Orderbook data collection service
│   ├── include/           # Header files
│   ├── src/               # Source files
│   ├── CMakeLists.txt
│   ├── Dockerfile
│   └── README.md
├── ppo-service/            # PPO neural network service
│   ├── include/           # Header files
│   ├── src/               # Source files
│   ├── CMakeLists.txt
│   ├── Dockerfile
│   └── README.md
├── oms-service/           # Order Management System
│   ├── include/           # Header files
│   ├── src/               # Source files
│   ├── CMakeLists.txt
│   ├── Dockerfile
│   └── README.md
├── docker-compose.yml     # Docker Compose configuration
└── README.md             # This file
```

## Performance Characteristics

- **Message Processing**: Event-driven with RabbitMQ consumer polling
- **Neural Network**: Double-precision (float64) computation
- **Orderbook Updates**: ~19,370 bytes per message
- **Trading Actions**: 18 bytes per message
- **State Buffer**: 80 states × 2,421 features
- **Memory Footprint**: ~100MB per service
- **Latency**: Optimized for low-latency processing with zero-copy operations

## Limitations

- Single trading pair support (BTC-USDT-SWAP)
- CPU-only inference (no GPU support)
- Fixed buffer sizes (80 states for PPO, 400 levels for orderbook)
- No dynamic hyperparameter tuning
- No persistence between restarts (except model checkpoints)
- State ID wraps around after 65535
- Abandoned project status - no active maintenance

## Development Notes

### Binary Message Encoding

The system uses custom binary encoding for efficient message passing:
- **Change values** (prices, imbalances): 1 bit sign + 63 bits fraction
- **Orderbook values** (volumes, orders): 1 bit sign + 10 bits whole + 53 bits fraction
- Zero values encoded as 0
- Precision threshold: 1e-15

See [`common/binary_utils.hpp`](common/binary_utils.hpp) for implementation details.

### Neural Network Training

The PPO service includes **continuous online training** capabilities:
- **Self-improving model**: Continuously learns from live trading data and reward signals
- **Online learning**: Model updates itself as it trades, adapting to changing market conditions
- PPO hyperparameters: clip epsilon 0.2, value coefficient 0.5, entropy coefficient 0.01
- Training buffer size: 100 trades
- Model save interval: Every 9000 states
- Mini-batch size: 16
- Learning rate: 0.0003
- The model learns from each trade execution, using PnL-based rewards and maximum drawdown to improve its trading strategy over time

## Contributing

This project is currently abandoned. If you wish to fork and continue development:
1. Review the individual service READMEs for detailed documentation
2. Ensure all dependencies are properly installed
3. Test thoroughly before deploying to production
4. Consider adding comprehensive error handling and monitoring

## License

MIT License - see [LICENSE](LICENSE) file for details.

## Disclaimer

**This software is provided for educational and research purposes only. Trading cryptocurrencies involves substantial risk of loss. The authors and contributors are not responsible for any financial losses incurred from using this software. Use at your own risk.**

## References

- [OKX API Documentation](https://www.okx.com/docs-v5/en/)
- [PyTorch C++ API (LibTorch)](https://pytorch.org/cppdocs/)
- [RabbitMQ Documentation](https://www.rabbitmq.com/documentation.html)
- [PPO Algorithm Paper](https://arxiv.org/abs/1707.06347)

