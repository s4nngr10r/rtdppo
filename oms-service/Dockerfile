FROM gcc:12.2.0

# Install dependencies
RUN apt-get update && apt-get install -y \
    cmake \
    librabbitmq-dev \
    netcat-openbsd \
    libssl-dev \
    nlohmann-json3-dev \
    libwebsockets-dev \
    && rm -rf /var/lib/apt/lists/*

# Create app directory
WORKDIR /app

# Copy source code first
COPY common/binary_utils.hpp /app/include/
COPY oms-service/src /app/src/
COPY oms-service/include /app/include/
COPY oms-service/CMakeLists.txt /app/

# Build the application
RUN mkdir -p build && cd build && \
    cmake .. && \
    make -j$(nproc) && \
    ls -la oms_service && \
    cp oms_service /app/

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
echo "RabbitMQ is ready. Starting OMS service..."\n\
sleep 5\n\
/app/oms_service' > /app/start.sh && chmod +x /app/start.sh

# Run the application
CMD ["/app/start.sh"] 