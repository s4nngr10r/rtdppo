#include "../include/rabbitmq_handler.hpp"
#include <iostream>

RabbitMQHandler::RabbitMQHandler(const std::string& host, int port,
                               const std::string& username, const std::string& password)
    : host_(host), port_(port), username_(username), password_(password), conn(nullptr) {}

RabbitMQHandler::~RabbitMQHandler() {
    if (conn) {
        amqp_channel_close(conn, 1, AMQP_REPLY_SUCCESS);
        amqp_connection_close(conn, AMQP_REPLY_SUCCESS);
        amqp_destroy_connection(conn);
    }
}

bool RabbitMQHandler::connect() {
    try {
        conn = amqp_new_connection();
        amqp_socket_t* socket = amqp_tcp_socket_new(conn);
        if (!socket) {
            throw std::runtime_error("Creating TCP socket failed");
        }

        int status = amqp_socket_open(socket, host_.c_str(), port_);
        if (status != AMQP_STATUS_OK) {
            throw std::runtime_error("Opening TCP socket failed");
        }

        auto login_status = amqp_login(conn, "/", 0, 131072, 0, AMQP_SASL_METHOD_PLAIN,
                                     username_.c_str(), password_.c_str());
        if (login_status.reply_type != AMQP_RESPONSE_NORMAL) {
            throw std::runtime_error("Login failed");
        }

        amqp_channel_open(conn, 1);
        auto channel_status = amqp_get_rpc_reply(conn);
        if (channel_status.reply_type != AMQP_RESPONSE_NORMAL) {
            throw std::runtime_error("Opening channel failed");
        }

        // Declare the exchange
        amqp_exchange_declare(conn, 1, 
                            amqp_cstring_bytes("orderbook"), // exchange name
                            amqp_cstring_bytes("topic"),     // exchange type
                            0,                               // passive
                            1,                               // durable
                            0,                               // auto_delete
                            0,                               // internal
                            amqp_empty_table);              // arguments

        auto exchange_status = amqp_get_rpc_reply(conn);
        if (exchange_status.reply_type != AMQP_RESPONSE_NORMAL) {
            throw std::runtime_error("Declaring exchange failed");
        }

        return true;
    } catch (const std::exception& e) {
        std::cerr << "RabbitMQ connection error: " << e.what() << std::endl;
        if (conn) {
            amqp_destroy_connection(conn);
            conn = nullptr;
        }
        return false;
    }
}

bool RabbitMQHandler::publishMessage(const std::string& exchange, const std::string& routingKey,
                                   const std::string& message) {
    if (!conn) {
        std::cerr << "Not connected to RabbitMQ" << std::endl;
        return false;
    }

    try {
        amqp_basic_properties_t props;
        props._flags = AMQP_BASIC_CONTENT_TYPE_FLAG | AMQP_BASIC_DELIVERY_MODE_FLAG;
        props.content_type = amqp_cstring_bytes("application/json");
        props.delivery_mode = 2; // persistent delivery mode

        int status = amqp_basic_publish(conn,
                                      1,
                                      amqp_cstring_bytes(exchange.c_str()),
                                      amqp_cstring_bytes(routingKey.c_str()),
                                      0,
                                      0,
                                      &props,
                                      amqp_cstring_bytes(message.c_str()));

        if (status != AMQP_STATUS_OK) {
            throw std::runtime_error("Publishing message failed");
        }

        return true;
    } catch (const std::exception& e) {
        std::cerr << "Error publishing message: " << e.what() << std::endl;
        return false;
    }
}

bool RabbitMQHandler::publishBinaryMessage(const std::string& exchange, const std::string& routingKey,
                                         const char* data, size_t size) {
    if (!conn) {
        std::cerr << "Not connected to RabbitMQ" << std::endl;
        return false;
    }

    try {
        amqp_basic_properties_t props;
        props._flags = AMQP_BASIC_CONTENT_TYPE_FLAG | AMQP_BASIC_DELIVERY_MODE_FLAG;
        props.content_type = amqp_cstring_bytes("application/octet-stream");
        props.delivery_mode = 2; // persistent delivery mode

        amqp_bytes_t message_bytes;
        message_bytes.len = size;
        message_bytes.bytes = const_cast<char*>(data);

        int status = amqp_basic_publish(conn,
                                      1,
                                      amqp_cstring_bytes(exchange.c_str()),
                                      amqp_cstring_bytes(routingKey.c_str()),
                                      0,
                                      0,
                                      &props,
                                      message_bytes);

        if (status != AMQP_STATUS_OK) {
            throw std::runtime_error("Publishing binary message failed");
        }

        return true;
    } catch (const std::exception& e) {
        std::cerr << "Error publishing binary message: " << e.what() << std::endl;
        return false;
    }
} 