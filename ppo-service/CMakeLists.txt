cmake_minimum_required(VERSION 3.10)
project(ppo-service)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_PREFIX_PATH ${CMAKE_PREFIX_PATH} "/usr/local/libtorch")

# Find pkg-config
find_package(PkgConfig REQUIRED)

# Find Torch
find_package(Torch REQUIRED)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${TORCH_CXX_FLAGS}")

# Use pkg-config to find libraries
pkg_check_modules(RABBITMQ REQUIRED librabbitmq)
pkg_check_modules(NLOHMANN_JSON REQUIRED nlohmann_json)

# Add source files
set(SOURCES
    src/main.cpp
    src/ppo_handler.cpp
)

# Add executable
add_executable(ppo-service ${SOURCES})

# Include directories
target_include_directories(ppo-service PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${RABBITMQ_INCLUDE_DIRS}
    ${NLOHMANN_JSON_INCLUDE_DIRS}
    ${TORCH_INCLUDE_DIRS}
)

# Link libraries
target_link_libraries(ppo-service PRIVATE
    ${RABBITMQ_LIBRARIES}
    ${NLOHMANN_JSON_LIBRARIES}
    ${TORCH_LIBRARIES}
    pthread
)

# Add compiler warnings
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(ppo-service PRIVATE -Wall -Wextra)
endif() 