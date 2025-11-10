#include "../include/oms_handler.hpp"
#include <cstdlib>
#include <iostream>
#include <string>

int main() {
    try {
        // Get RabbitMQ connection details from environment variables
        std::string host = std::getenv("RABBITMQ_HOST") ? std::getenv("RABBITMQ_HOST") : "localhost";
        int port = std::getenv("RABBITMQ_PORT") ? std::stoi(std::getenv("RABBITMQ_PORT")) : 5672;
        std::string username = std::getenv("RABBITMQ_USERNAME") ? std::getenv("RABBITMQ_USERNAME") : "guest";
        std::string password = std::getenv("RABBITMQ_PASSWORD") ? std::getenv("RABBITMQ_PASSWORD") : "guest";

        // Get OKX API credentials from environment variables
        const char* okx_api_key = std::getenv("OKX_API_KEY");
        const char* okx_secret_key = std::getenv("OKX_SECRET_KEY");
        const char* okx_passphrase = std::getenv("OKX_PASSPHRASE");

        if (!okx_api_key || !okx_secret_key || !okx_passphrase) {
            throw std::runtime_error("Missing required OKX credentials in environment variables");
        }

        // Create and start OMS handler
        OMSHandler handler(host, port, username, password,
                         okx_api_key, okx_secret_key, okx_passphrase);
        
        std::cout << "Starting OMS service..." << std::endl;
        std::cout << "RabbitMQ connection details:" << std::endl;
        std::cout << "  Host: " << host << std::endl;
        std::cout << "  Port: " << port << std::endl;
        std::cout << "  Username: " << username << std::endl;
        std::cout << "OKX WebSocket connection initialized." << std::endl;
        
        handler.start();
        
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << std::endl;
        return 1;
    }
} 