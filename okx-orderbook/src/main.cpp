#include "../include/websocket_client.hpp"
#include "../include/orderbook_handler.hpp"
#include "../include/rabbitmq_handler.hpp"
#include <iostream>
#include <thread>
#include <cstdlib>
#include <csignal>

// Helper function to get environment variable with default value
std::string getEnvVar(const char* name, const std::string& defaultValue) {
    const char* value = std::getenv(name);
    return value ? value : defaultValue;
}

int main() {
    try {
        // Get RabbitMQ connection parameters from environment variables
        std::string rmqHost = getEnvVar("RABBITMQ_HOST", "localhost");
        int rmqPort = std::stoi(getEnvVar("RABBITMQ_PORT", "5672"));
        std::string rmqUser = getEnvVar("RABBITMQ_USER", "guest");
        std::string rmqPass = getEnvVar("RABBITMQ_PASS", "guest");

        std::cout << "Connecting to RabbitMQ at " << rmqHost << ":" << rmqPort << std::endl;
        
        // Initialize RabbitMQ handler with environment variables
        RabbitMQHandler rmq(rmqHost, rmqPort, rmqUser, rmqPass);
        if (!rmq.connect()) {
            std::cerr << "Failed to connect to RabbitMQ" << std::endl;
            return 1;
        }

        std::cout << "Successfully connected to RabbitMQ" << std::endl;

        // Initialize WebSocket client
        WebSocketClient client("ws.okx.com", "wss");
        OrderBookHandler orderbook(&client, &rmq);

        // Set message callback
        client.setMessageCallback([&orderbook](const std::string& msg) {
            orderbook.handleMessage(msg);
        });

        std::cout << "Connecting to WebSocket..." << std::endl;
        
        // Connect to WebSocket
        if (!client.connect()) {
            std::cerr << "Failed to connect to WebSocket server" << std::endl;
            return 1;
        }

        std::cout << "Successfully connected to WebSocket" << std::endl;

        // Subscribe to orderbook
        orderbook.subscribe("BTC-USDT-SWAP");

        // Run WebSocket client on a separate thread
        std::thread clientThread([&client]() {
            client.run();
        });

        // Schedule ping messages on a separate thread
        std::thread pingThread([&client]() {
            client.schedulePing();
        });

        // Keep threads running
        clientThread.join();
        pingThread.join();

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
