cmake_minimum_required(VERSION 3.10)
project(okx_orderbook)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find required packages
find_package(OpenSSL REQUIRED)
find_package(Boost REQUIRED COMPONENTS system)
find_package(PkgConfig REQUIRED)
pkg_check_modules(WEBSOCKETS REQUIRED libwebsockets)
pkg_check_modules(NLOHMANN_JSON REQUIRED nlohmann_json)
pkg_check_modules(RABBITMQ REQUIRED librabbitmq)
find_package(Threads REQUIRED)

# Find simdjson
find_package(simdjson REQUIRED)

# Add source files
add_executable(okx_orderbook
    src/main.cpp
    src/websocket_client.cpp
    src/orderbook_handler.cpp
    src/rabbitmq_handler.cpp
)

# Include directories
target_include_directories(okx_orderbook PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${WEBSOCKETS_INCLUDE_DIRS}
    ${NLOHMANN_JSON_INCLUDE_DIRS}
    ${RABBITMQ_INCLUDE_DIRS}
)

# Link libraries
target_link_libraries(okx_orderbook PRIVATE
    OpenSSL::SSL
    OpenSSL::Crypto
    Boost::system
    ${WEBSOCKETS_LIBRARIES}
    ${NLOHMANN_JSON_LIBRARIES}
    ${RABBITMQ_LIBRARIES}
    simdjson
    Threads::Threads
)

# Add compiler warnings
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(okx_orderbook PRIVATE -Wall -Wextra)
endif()