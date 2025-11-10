#include "../include/ppo_handler.hpp"
#include <iostream>
#include <cstdlib>
#include <csignal>
#include <string>

// Global PPO handler for signal handling
PPOHandler* g_ppo_handler = nullptr;

void signalHandler(int signum) {
    std::cout << "\nSignal (" << signum << ") received. Cleaning up..." << std::endl;
    if (g_ppo_handler) {
        g_ppo_handler->stop();
    }
    exit(signum);
}

int main() {
    try {
        // Register signal handler
        signal(SIGINT, signalHandler);
        signal(SIGTERM, signalHandler);

        // Get RabbitMQ connection details from environment variables
        std::string host = std::getenv("RABBITMQ_HOST") ? std::getenv("RABBITMQ_HOST") : "localhost";
        int port = std::getenv("RABBITMQ_PORT") ? std::stoi(std::getenv("RABBITMQ_PORT")) : 5672;
        std::string username = std::getenv("RABBITMQ_USERNAME") ? std::getenv("RABBITMQ_USERNAME") : "guest";
        std::string password = std::getenv("RABBITMQ_PASSWORD") ? std::getenv("RABBITMQ_PASSWORD") : "guest";

        // Create and start PPO handler
        PPOHandler ppo(host, port, username, password);
        g_ppo_handler = &ppo;

        std::cout << "Starting PPO service..." << std::endl;
        ppo.start();

        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
} 