FROM gcc:12.2.0

# Install dependencies
RUN apt-get update && apt-get install -y \
    cmake \
    librabbitmq-dev \
    nlohmann-json3-dev \
    netcat-openbsd \
    wget \
    unzip \
    && rm -rf /var/lib/apt/lists/*

# Download and install LibTorch with CPU support
RUN wget https://download.pytorch.org/libtorch/cpu/libtorch-cxx11-abi-shared-with-deps-2.1.0%2Bcpu.zip \
    && unzip libtorch-cxx11-abi-shared-with-deps-2.1.0+cpu.zip -d /usr/local \
    && rm libtorch-cxx11-abi-shared-with-deps-2.1.0+cpu.zip

# Set environment variables for LibTorch
ENV LIBTORCH=/usr/local/libtorch
ENV LD_LIBRARY_PATH=${LIBTORCH}/lib:$LD_LIBRARY_PATH

WORKDIR /app
RUN mkdir -p include
COPY common/binary_utils.hpp include/
COPY ppo-service/ .

# Create startup script
RUN echo '#!/bin/bash\n\
MAX_TRIES=30\n\
RETRY_INTERVAL=2\n\
\n\
echo "Waiting for RabbitMQ to be ready..."\n\
\n\
count=0\n\
while ! nc -z $RABBITMQ_HOST $RABBITMQ_PORT; do\n\
    echo "Attempt $count: RabbitMQ is not ready yet..."\n\
    sleep $RETRY_INTERVAL\n\
    count=$((count + 1))\n\
    if [ $count -ge $MAX_TRIES ]; then\n\
        echo "Max retries reached. RabbitMQ is not available."\n\
        exit 1\n\
    fi\n\
done\n\
\n\
echo "RabbitMQ is ready. Starting PPO service..."\n\
sleep 5\n\
./build/ppo-service' > /app/start.sh && chmod +x /app/start.sh

# Build the application with proper LibTorch path
RUN mkdir build && cd build && \
    cmake -DCMAKE_PREFIX_PATH=/usr/local/libtorch .. && \
    make

# Run the application
CMD ["/app/start.sh"] 