#pragma once
#include <string>
#include <amqp.h>
#include <amqp_tcp_socket.h>
#include <nlohmann/json.hpp>

class RabbitMQHandler {
public:
    RabbitMQHandler(const std::string& host, int port,
                    const std::string& username, const std::string& password);
    ~RabbitMQHandler();

    bool connect();
    bool publishMessage(const std::string& exchange, const std::string& routingKey,
                       const std::string& message);
    bool publishBinaryMessage(const std::string& exchange, const std::string& routingKey,
                            const char* data, size_t size);

private:
    std::string host_;
    int port_;
    std::string username_;
    std::string password_;
    amqp_connection_state_t conn;
}; 