#include "../include/websocket_client.hpp"
#include <iostream>
#include <openssl/ssl.h>
#include <nlohmann/json.hpp>
#include <chrono>
#include <thread>

WebSocketClient* WebSocketClient::instance_ = nullptr;

WebSocketClient::WebSocketClient(const std::string& url, const std::string& protocol)
    : url_(url), protocol_(protocol), context_(nullptr), connection_(nullptr) {
    instance_ = this;
}

WebSocketClient::~WebSocketClient() {
    if (context_) {
        lws_context_destroy(context_);
    }
}

bool WebSocketClient::connect() {
    struct lws_context_creation_info info;
    memset(&info, 0, sizeof(info));

    // Initialize OpenSSL
    SSL_library_init();
    OpenSSL_add_all_algorithms();
    SSL_load_error_strings();

    info.port = CONTEXT_PORT_NO_LISTEN;
    info.protocols = new lws_protocols[2]{
        {
            "wss",  // Use wss protocol
            callback_function,
            0,
            262144,  // Increased rx buffer size
            0,
            nullptr,
            0
        },
        { nullptr, nullptr, 0, 0, 0, nullptr, 0 }
    };
    info.gid = -1;
    info.uid = -1;
    info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
    info.client_ssl_cert_filepath = nullptr;
    info.client_ssl_private_key_filepath = nullptr;
    info.client_ssl_ca_filepath = nullptr;
    info.ssl_cipher_list = "HIGH:!aNULL:!MD5:!RC4";
    info.ssl_options_set = SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3;
    info.ssl_options_clear = 0;

    context_ = lws_create_context(&info);
    if (!context_) {
        std::cerr << "Failed to create WebSocket context" << std::endl;
        return false;
    }

    struct lws_client_connect_info ccinfo;
    memset(&ccinfo, 0, sizeof(ccinfo));

    ccinfo.context = context_;
    ccinfo.address = "ws.okx.com";  // Direct hostname
    ccinfo.port = 443;
    ccinfo.path = "/ws/v5/public";
    ccinfo.host = "ws.okx.com";
    ccinfo.origin = "ws.okx.com";
    ccinfo.protocol = "wss";
    ccinfo.ssl_connection = LCCSCF_USE_SSL | LCCSCF_ALLOW_SELFSIGNED | LCCSCF_SKIP_SERVER_CERT_HOSTNAME_CHECK;
    ccinfo.alpn = "http/1.1";
    ccinfo.local_protocol_name = "wss";

    std::cout << "Connecting to WebSocket server: " << ccinfo.address << ccinfo.path << std::endl;
    
    connection_ = lws_client_connect_via_info(&ccinfo);
    if (!connection_) {
        std::cerr << "Failed to connect to WebSocket server" << std::endl;
        return false;
    }

    return true;
}

void WebSocketClient::run() {
    while (1) {
        lws_service(context_, 50);
    }
}

void WebSocketClient::send(const std::string& message) {
    if (connection_) {
        // Add LWS_PRE bytes for libwebsockets header
        unsigned char* buf = new unsigned char[LWS_PRE + message.length()];
        memcpy(&buf[LWS_PRE], message.c_str(), message.length());
        
        lws_write(connection_, &buf[LWS_PRE], message.length(), LWS_WRITE_TEXT);
        delete[] buf;
    }
}

void WebSocketClient::setMessageCallback(std::function<void(const std::string&)> callback) {
    message_callback_ = callback;
}

int WebSocketClient::callback_function(struct lws* wsi, enum lws_callback_reasons reason,
                                     void* user, void* in, size_t len) {
    switch (reason) {
        case LWS_CALLBACK_CLIENT_ESTABLISHED: {
            std::cout << "Connected to server" << std::endl;
            if (instance_) {
                instance_->message_buffer_.clear();
                if (instance_->pending_subscribe_message_.length() > 0) {
                    instance_->send(instance_->pending_subscribe_message_);
                }
            }
            break;
        }
        case LWS_CALLBACK_CLIENT_RECEIVE: {
            if (instance_ && instance_->message_callback_) {
                if (in && len > 0) {
                    // Get final fragment flag
                    bool is_final = lws_is_final_fragment(wsi);
                    bool is_start = lws_is_first_fragment(wsi);
                    
                    // Append to buffer
                    if (is_start) {
                        instance_->message_buffer_.clear();
                    }
                    
                    instance_->message_buffer_.append(static_cast<char*>(in), len);

                    // Process complete message
                    if (is_final) {
                        std::string complete_message = instance_->message_buffer_;
                        instance_->message_buffer_.clear();

                        try {
                            // Validate JSON before passing to callback
                            auto j = nlohmann::json::parse(complete_message);
                            instance_->message_callback_(complete_message);
                        } catch (const std::exception& e) {
                            std::cerr << "Invalid JSON in websocket message: " << e.what() << std::endl;
                        }
                    }
                }
            }
            break;
        }
        case LWS_CALLBACK_CLIENT_CLOSED:
            std::cout << "Connection closed" << std::endl;
            break;
        case LWS_CALLBACK_CLIENT_CONNECTION_ERROR: {
            std::string error_msg;
            if (in) {
                error_msg = std::string(static_cast<char*>(in), len);
            } else {
                error_msg = "Unknown error";
            }
            std::cerr << "Connection error: " << error_msg << std::endl;
            break;
        }
        default:
            break;
    }
    return 0;
}

void WebSocketClient::schedulePing() {
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(30));
        sendPing();
    }
}

void WebSocketClient::sendPing() {
    if (connection_) {
        nlohmann::json pingMessage = {{"op", "ping"}};
        std::string pingString = pingMessage.dump();
        unsigned char* buf = new unsigned char[LWS_PRE + pingString.length()];
        memcpy(&buf[LWS_PRE], pingString.c_str(), pingString.length());
        
        lws_write(connection_, &buf[LWS_PRE], pingString.length(), LWS_WRITE_TEXT);
        delete[] buf;
    }
}
