#pragma once
#include <string>
#include <functional>
#include <memory>
#include <libwebsockets.h>

class WebSocketClient {
public:
    WebSocketClient(const std::string& url, const std::string& protocol);
    ~WebSocketClient();

    bool connect();
    void run();
    void send(const std::string& message);
    void setMessageCallback(std::function<void(const std::string&)> callback);
    void setPendingSubscribeMessage(const std::string& message) { 
        pending_subscribe_message_ = message; 
    }
    void schedulePing();

private:
    static int callback_function(struct lws* wsi, enum lws_callback_reasons reason,
                               void* user, void* in, size_t len);

    std::string url_;
    std::string protocol_;
    struct lws_context* context_;
    struct lws* connection_;
    std::function<void(const std::string&)> message_callback_;
    std::string pending_subscribe_message_;
    std::string message_buffer_;
    
    static WebSocketClient* instance_;
    void sendPing();
};
