cmake_minimum_required(VERSION 3.10)
project(oms_service)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find required packages
find_package(OpenSSL REQUIRED)
find_package(nlohmann_json 3.2.0 REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBWEBSOCKETS REQUIRED libwebsockets)

# Add include directories
include_directories(${CMAKE_SOURCE_DIR}/include)

# Add executable
add_executable(oms_service
    src/main.cpp
    src/oms_handler.cpp
    src/okx_websocket.cpp
    src/possizehandler.cpp
)

# Link libraries
target_link_libraries(oms_service
    PRIVATE
    OpenSSL::SSL
    OpenSSL::Crypto
    nlohmann_json::nlohmann_json
    ${LIBWEBSOCKETS_LIBRARIES}
    rabbitmq
    pthread
) 