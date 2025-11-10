# OMS Service

A high-performance, event-driven order management system for cryptocurrency trading on the OKX exchange, operating in cross-margin mode.

## Technical Architecture

### Core Components
- **OMSHandler**: Main service coordinator managing trade state and message processing
- **OKXWebSocket**: Real-time market connection with automatic reconnection
- **Trade Management**: Position and order tracking with reward calculation
- **Order Processing**: Thread-safe order management with deque-based queue
- **Execution Reporting**: Real-time trade execution updates with reward metrics

### Key Features
- Event-driven architecture with thread-safe operations
- Real-time WebSocket communication with automatic reconnection (max 50 retries)
- Automatic trade closure with reward calculation
- Position mode: Net
- Execution tracking and reporting
- Maximum drawdown tracking (atomic)
- PnL-based reward calculation
- Thread-safe order management with mutex protection

## Operational Flow

```mermaid
graph TD
    %% Main Components
    subgraph Input Processing
        Input[Binary Input 25 bytes] -->|4 bytes state_id + 21 bytes data| Decoder[Message Decoder]
        Decoder -->|Decoded Parameters| Calculator[Order Calculator]
    end

    subgraph Balance Management
        Balance[Account Balance] -->|Atomic Balance Access| Calculator
        Balance -->|Cross-Margin Mode| Parameters[Trading Parameters]
        Parameters -->|"Leverage(100x)"| OrderCreation[Order Creation]
    end

    subgraph WebSocket Management
        WebSocket[OKX WebSocket] -->|HMAC Auth| WSAuth[API Authentication]
        WSAuth -->|"API Key, Secret, Passphrase"| WSChannels[WebSocket Channels]
        WSChannels -->|Subscribe| Subscriptions[Channel Subscriptions]
        
        Subscriptions -->|orders| OrderUpdates[Order Updates]
        Subscriptions -->|account| Balance
        Subscriptions -->|positions| PositionUpdates[Position Updates]
        
        WebSocket -->|Connection Lost| Reconnect[Auto Reconnect]
        Reconnect -->|"Max 50 Retries<br/>65536 RX Buffer"| WSAuth
    end

    subgraph Order Processing
        OrderCreation -->|New Order| OrderQueue[Thread-Safe Order Deque]
        OrderQueue -->|Mutex Lock| Store[Store Order]
        Store -->|Update State| OrderTracking[Order State Tracking]
        
        OrderUpdates -->|Validation| ErrorCheck{Error Check}
        ErrorCheck -->|Invalid| ErrorHandler[Error Handler]
        ErrorCheck -->|Valid| StateHandler[State Handler]
        
        StateHandler -->|live| LiveOrder[Live Order]
        StateHandler -->|partially_filled or filled| FillHandler[Fill Handler]
        
        FillHandler -->|Atomic Update| OrderUpdate[Update Order in Trade]
        OrderUpdate -->|Check State| TradeCheck{Active Trade?}
    end

    subgraph Trade Management
        TradeCheck -->|No| NewTrade[Create New Trade]
        TradeCheck -->|Yes| DirectionCheck{Same Direction?}
        
        DirectionCheck -->|Yes| IncreasePosition[Increase Position]
        DirectionCheck -->|No| DecreasePosition[Decrease Position]
        
        IncreasePosition -->|"Mutex Protected"| UpdateTradeSize[Update Trade Size]
        DecreasePosition -->|"Mutex Protected"| UpdateTradeSize
        DecreasePosition -->|"Calculate PnL"| UpdateReward[Update Cumulative Reward]
        UpdateReward -->|"Atomic Update"| SizeCheck{Size <= 0?}
    end

    subgraph Position Management
        PositionUpdates -->|Update| PositionState[Position State]
        PositionState -->|"Atomic Update"| MaxDrawdown[Update Max Drawdown]
        PositionState -->|Net Mode| PositionTracking[Position Tracking]
        
        SizeCheck -->|Yes| CloseTrade[Close Trade]
        SizeCheck -->|No| UpdateTradeSize
    end

    subgraph Queue Maintenance
        OrderTracking -->|Old Orders| CancelRequest[Cancel Request]
        CancelRequest -->|Track State| CancellationInfo[Cancellation Info]
        CancellationInfo -->|Mutex Protected| WebSocket
        
        Store -->|"Thread-safe<br/>Processing"| QueueMaintenance[Queue Maintenance]
        QueueMaintenance -->|Cleanup| RemoveFilled[Remove Filled Orders]
        QueueMaintenance -->|Vector Storage| OldOrders[Old Orders Vector]
    end

    subgraph Execution Reporting
        UpdateTradeSize -->|Any Fill| SingleUpdate[Order Update]
        CloseTrade -->|Final Calculation| RewardCalc[Reward Calculation]
        RewardCalc -->|"MaxDD Impact"| ClosureUpdate[Trade Closure Update]
        
        SingleUpdate -->|"state_id, okx_id"| RMQPublish[RabbitMQ Publish]
        ClosureUpdate -->|"state_id, okx_id,<br/>portions, reward"| RMQPublish
        
        RMQPublish -->|Topic Exchange| RMQRoute[Topic Routing]
        RMQRoute -->|execution.update| RMQConsumers[Consumers]
    end

    %% Style Definitions
    classDef process fill:#f9f,stroke:#333,stroke-width:2px
    classDef decision fill:#bbf,stroke:#333,stroke-width:2px
    classDef data fill:#ffa,stroke:#333,stroke-width:2px
    classDef websocket fill:#bfb,stroke:#333,stroke-width:2px
    classDef atomic fill:#ffd700,stroke:#333,stroke-width:2px
    classDef mutex fill:#98fb98,stroke:#333,stroke-width:2px

    %% Apply Styles
    class Calculator,OrderCreation,Store,QueueMaintenance,RewardCalc process
    class TradeCheck,DirectionCheck,SizeCheck,ErrorCheck decision
    class Balance,OrderQueue,PositionState data
    class WebSocket,WSChannels,OrderUpdates websocket
    class MaxDrawdown,UpdateReward,Balance atomic
    class Store,UpdateTradeSize,CancellationInfo mutex

```

## Message Processing

### Input Format
- Binary format (25 bytes)
- 4 bytes: State ID
- 21 bytes: Action data (V2 format)

### Order States
- live
- partially_filled
- filled
- canceled

### Order Information Tracking
```cpp
struct OrderInfo {
    uint32_t state_id;           // Internal state ID from input
    double volume;               // Original order volume
    double price;                // Original order price
    std::string okx_order_id;    // OKX order ID
    bool has_okx_id;             // Flag indicating if OKX ID is set
    double filled_size;          // Actual filled size
    double avg_fill_price;       // Average fill price
    bool is_filled;              // Whether the order is filled
    double execution_percentage;  // Track percentage of order executed
    std::string order_state;     // Track order state
}
```

### Execution Updates
Two types of execution messages are published:

1. **Order Execution**
```json
{
    "state_id": <uint32>,
    "okx_id": "<string>",
    "is_trade_closed": false
}
```

2. **Trade Closure**
```json
{
    "state_id": <uint32>,
    "okx_id": "<string>",
    "is_trade_closed": true,
    "filled_portions": [
        {"<okx_id>": <execution_percentage>},
        ...
    ],
    "reward": <double>
}
```

Note: All fills (partial or complete) generate an execution message, except when transitioning from partially filled to fully executed state.

## Trade Management

### Position Tracking
- Net position mode
- Automatic direction switching
- Size accumulation/reduction
- Execution percentage tracking
- Atomic maximum drawdown monitoring
- Cumulative reward calculation

### Reward Calculation
- Tracks cumulative reward during trade lifecycle
- Considers PnL for each position reduction
- Final reward calculation:
  - For positive cumulative reward: reward = cumulative_reward * (1 - 2 * |maxdd|)
  - For negative cumulative reward: reward = cumulative_reward * (1 + 2 * |maxdd|)
- Incorporates maximum drawdown impact on final reward

### Order Lifecycle
1. Order creation and thread-safe queueing
2. WebSocket confirmation with retry mechanism
3. Execution tracking with percentage calculation
4. Trade closure handling
5. Reward calculation
6. Execution reporting

## WebSocket Integration

### Connection Details
- Host: ws.okx.com
- Port: 8443
- Path: /ws/v5/private
- Protocol: ws
- RX Buffer Size: 65536
- Max Retries: 50

### Channels
- orders
- positions
- account

### Authentication
- API Key
- Secret Key
- Passphrase

## Thread Safety & Concurrency

### Mutex Protection
- Orders mutex for deque access
- Old orders mutex for cancellation tracking
- WebSocket connection mutex
- Balance atomic variables

### Atomic Operations
- Initial balance tracking
- Connection state
- Maximum drawdown updates

## Risk Management

### Order Controls
- Thread-safe order processing
- Automatic cancellation tracking
- Maximum drawdown monitoring (atomic)
- Execution percentage tracking

### Queue Management
- Thread-safe deque operations
- Automatic cleanup of filled orders
- Cancellation state tracking
- Old order processing

## Configuration

### Environment Variables
- OKX_API_KEY
- OKX_SECRET_KEY
- OKX_PASSPHRASE
- RABBITMQ_HOST
- RABBITMQ_PORT
- RABBITMQ_USER
- RABBITMQ_PASS

### Trading Parameters
- Leverage: 100x
- Minimum Contract Size: 0.1
- Trading Pair: BTC-USDT-SWAP

## Operational Notes

### Cross-Margin Mode
- Operating in cross-margin mode
- Automatic margin calculation
- Balance-based position sizing

### Position Management
- Net position tracking
- Automatic trade closure
- Direction switching support
- Partial fill support
- Atomic maximum drawdown monitoring
- Thread-safe order processing 