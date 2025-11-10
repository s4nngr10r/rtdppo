#pragma once
#include <string>
#include <deque>
#include <amqp.h>
#include <amqp_tcp_socket.h>
#include <chrono>
#include <torch/torch.h>
#include <memory>
#include <iomanip>
#include <nlohmann/json.hpp>
#include "orderbook_state.hpp"

// Actor network architecture (using float64/double precision)
class ActorImpl : public torch::nn::Module {
public:
    ActorImpl(int input_size)
        : conv1(register_module("conv1", torch::nn::Conv1d(torch::nn::Conv1dOptions(2421, 128, 3).padding(1)))),
          conv2(register_module("conv2", torch::nn::Conv1d(torch::nn::Conv1dOptions(128, 64, 3).padding(1)))),
          lstm(register_module("lstm", torch::nn::LSTM(torch::nn::LSTMOptions(64, 32).num_layers(2).batch_first(true)))),
          fc1(register_module("fc1", torch::nn::Linear(32 * 80, 128))),
          fc2(register_module("fc2", torch::nn::Linear(128, 64))),
          price_head(register_module("price_head", torch::nn::Linear(64, 1))),
          volume_head(register_module("volume_head", torch::nn::Linear(64, 1))) {
        
        // Initialize price_head with smaller weights
        double k = 1.0 / std::sqrt(64.0);  // Xavier/Glorot initialization scale
        torch::nn::init::uniform_(price_head->weight, -k, k);
        torch::nn::init::zeros_(price_head->bias);
        
        // Convert the entire module to double precision
        this->to(torch::kFloat64);
    }

    std::tuple<torch::Tensor, torch::Tensor> forward(torch::Tensor x) {
        // x shape: [batch_size, sequence_length=80, features=2421]
        x = x.transpose(1, 2);  // [batch_size, features=2421, sequence_length=80]
        
        // Apply 1D convolutions
        x = torch::relu(conv1->forward(x));  // [batch_size, 128, 80]
        x = torch::relu(conv2->forward(x));  // [batch_size, 64, 80]
        
        // Prepare for LSTM
        x = x.transpose(1, 2);  // [batch_size, 80, 64]
        
        // Apply LSTM
        auto lstm_out = std::get<0>(lstm->forward(x));  // [batch_size, 80, 32]
        
        // Flatten the sequence
        x = lstm_out.reshape({lstm_out.size(0), -1});  // [batch_size, 80 * 32]
        
        // Dense layers
        x = torch::relu(fc1->forward(x));
        x = torch::relu(fc2->forward(x));
        
        auto price = torch::tanh(price_head->forward(x));
        auto volume = torch::sigmoid(volume_head->forward(x));
        
        return std::make_tuple(price, volume);
    }

private:
    torch::nn::Conv1d conv1, conv2;
    torch::nn::LSTM lstm;
    torch::nn::Linear fc1, fc2, price_head, volume_head;
};
TORCH_MODULE(Actor);

// Critic network architecture (using float64/double precision)
class CriticImpl : public torch::nn::Module {
public:
    CriticImpl(int input_size)
        : conv1(register_module("conv1", torch::nn::Conv1d(torch::nn::Conv1dOptions(2421, 128, 3).padding(1)))),
          conv2(register_module("conv2", torch::nn::Conv1d(torch::nn::Conv1dOptions(128, 64, 3).padding(1)))),
          lstm(register_module("lstm", torch::nn::LSTM(torch::nn::LSTMOptions(64, 32).num_layers(2).batch_first(true)))),
          fc1(register_module("fc1", torch::nn::Linear(32 * 80, 128))),
          fc2(register_module("fc2", torch::nn::Linear(128, 64))),
          value_head(register_module("value_head", torch::nn::Linear(64, 1))) {
        
        // Convert the entire module to double precision
        this->to(torch::kFloat64);
    }

    torch::Tensor forward(torch::Tensor x) {
        // x shape: [batch_size, sequence_length=80, features=2421]
        x = x.transpose(1, 2);  // [batch_size, features=2421, sequence_length=80]
        
        // Apply 1D convolutions
        x = torch::relu(conv1->forward(x));  // [batch_size, 128, 80]
        x = torch::relu(conv2->forward(x));  // [batch_size, 64, 80]
        
        // Prepare for LSTM
        x = x.transpose(1, 2);  // [batch_size, 80, 64]
        
        // Apply LSTM
        auto lstm_out = std::get<0>(lstm->forward(x));  // [batch_size, 80, 32]
        
        // Flatten the sequence
        x = lstm_out.reshape({lstm_out.size(0), -1});  // [batch_size, 80 * 32]
        
        // Dense layers
        x = torch::relu(fc1->forward(x));
        x = torch::relu(fc2->forward(x));
        
        return value_head->forward(x);
    }

private:
    torch::nn::Conv1d conv1, conv2;
    torch::nn::LSTM lstm;
    torch::nn::Linear fc1, fc2, value_head;
};
TORCH_MODULE(Critic);

// Action storage structure
struct ActionInfo {
    double price;
    double volume;
    uint16_t state_id;
};

// Order tracking structure within a trade
struct OrderInfo {
    std::vector<uint16_t> state_ids;  // Array of 80 state IDs (stateid-79 to stateid)
    ActionInfo action;                 // Action that triggered this order
    std::string okx_id;               // OKX order ID
    double coefficient;               // Order coefficient
};

// Trade structure
struct TradeInfo {
    double reward;                    // Trade reward
    std::vector<OrderInfo> orders;    // Array of orders in this trade
};

class PPOHandler {
public:
    static constexpr size_t NETWORK_INPUT_SIZE = 80;  // Use 80 newest states for network input
    static constexpr size_t HISTORY_BUFFER_SIZE = 1000;  // Store last 1000 states
    static constexpr size_t ACTION_BUFFER_SIZE = 1000;  // Store last 1000 actions
    static constexpr size_t INPUT_SIZE = OrderBookState::TOTAL_FEATURES * NETWORK_INPUT_SIZE;
    static constexpr size_t SAVE_INTERVAL = 9000;  // Save model every 9000 states

    PPOHandler(const std::string& host, int port, 
               const std::string& username, const std::string& password);
    ~PPOHandler();

    void start();
    void stop();

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

    // Optimized state buffer (using double precision)
    std::deque<OrderBookState> state_buffer_;
    uint16_t trigger_state_id_;  // ID of the state that triggered action

    // PPO Networks and optimizers (using float64/double precision)
    Actor actor_;
    Critic critic_;
    std::unique_ptr<torch::optim::Adam> actor_optimizer_;
    std::unique_ptr<torch::optim::Adam> critic_optimizer_;

    // PPO Hyperparameters
    const double clip_epsilon_ = 0.2;
    const double value_coef_ = 0.5;
    const double entropy_coef_ = 0.01;
    const int ppo_epochs_ = 2;
    const int mini_batch_size_ = 16;
    const double learning_rate_ = 0.0003;

    // Action history buffer
    std::deque<ActionInfo> action_buffer_;

    // Trade tracking
    TradeInfo current_trade_;

    // Private methods
    void initializeRabbitMQ();
    void cleanupRabbitMQ();
    void handleMessage(const std::string& message);
    void handleExecutionUpdate(const std::string& message);
    std::string getCurrentTimestamp() const;
    void processOrderBookState(const OrderBookState& state);
    
    // PPO methods (all operating in double precision)
    torch::Tensor preprocessState();
    void forwardPass();
    void publishAction(const torch::Tensor& price, const torch::Tensor& volume);
    
    // Helper methods
    void initializeNetworks();
    void declareExchangesAndQueues();

    // PPO Training methods
    void updateNetworks(const TradeInfo& completed_trade);
    torch::Tensor computeAdvantages(const std::vector<torch::Tensor>& states,
                                   const std::vector<torch::Tensor>& values,
                                   const std::vector<double>& coefficients,
                                   double reward);
    torch::Tensor computePPOLoss(const torch::Tensor& advantages,
                                const torch::Tensor& old_price_probs,
                                const torch::Tensor& old_volume_probs,
                                const torch::Tensor& new_price_probs,
                                const torch::Tensor& new_volume_probs,
                                const std::vector<double>& coefficients);
    torch::Tensor computeValueLoss(const torch::Tensor& values,
                                  const torch::Tensor& returns,
                                  const std::vector<double>& coefficients);
    torch::Tensor computeEntropyLoss(const torch::Tensor& price_probs,
                                    const torch::Tensor& volume_probs,
                                    const std::vector<double>& coefficients);
    std::vector<torch::Tensor> getStatesFromTrade(const TradeInfo& trade);
    std::tuple<torch::Tensor, torch::Tensor> getActionsFromTrade(const TradeInfo& trade);
    std::vector<torch::Tensor> getValuesFromStates(const std::vector<torch::Tensor>& states);
    std::tuple<torch::Tensor, torch::Tensor> getProbabilitiesFromStates(const std::vector<torch::Tensor>& states);
    
    // Training buffers
    std::vector<TradeInfo> training_buffer_;
    const size_t MAX_TRAINING_BUFFER_SIZE = 100;  // Store last 100 trades for training

    // Model saving/loading
    void saveModel(const std::string& reason = "interval");
    bool loadModel();
    std::string getModelPath() const;
    size_t state_counter_;  // Count processed states for save interval
    const std::string MODEL_DIR = "models";  // Directory for model storage
}; 