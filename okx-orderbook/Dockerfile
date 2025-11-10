FROM gcc:12.2.0

# Install dependencies
RUN apt-get update && apt-get install -y \
    cmake \
    libssl-dev \
    libboost-system-dev \
    libwebsockets-dev \
    nlohmann-json3-dev \
    librabbitmq-dev \
    netcat-openbsd \
    git \
    && rm -rf /var/lib/apt/lists/*

# Install simdjson
RUN git clone https://github.com/simdjson/simdjson.git \
    && cd simdjson \
    && mkdir build \
    && cd build \
    && cmake .. \
    && make -j$(nproc) \
    && make install \
    && cd ../.. \
    && rm -rf simdjson

WORKDIR /app
RUN mkdir -p include
COPY common/binary_utils.hpp include/
COPY okx-orderbook/ .

# Create wait-for-rabbitmq script
RUN echo '#!/bin/bash\n\
MAX_TRIES=30\n\
RETRY_INTERVAL=2\n\
\n\
echo "Waiting for RabbitMQ to be ready..."\n\
\n\
count=0\n\
while ! nc -z $RABBITMQ_HOST $RABBITMQ_PORT; do\n\
    echo "RabbitMQ is unavailable - sleeping"\n\
    sleep $RETRY_INTERVAL\n\
    count=$((count+1))\n\
    if [ $count -eq $MAX_TRIES ]; then\n\
        echo "RabbitMQ did not become available in time - exiting"\n\
        exit 1\n\
    fi\n\
done\n\
\n\
echo "RabbitMQ is up - executing command"\n\
exec "$@"' > /wait-for-rabbitmq.sh \
    && chmod +x /wait-for-rabbitmq.sh

# Build the project
RUN mkdir build && cd build && \
    cmake .. && \
    make

# Set the entry point to wait for RabbitMQ before starting
ENTRYPOINT ["/wait-for-rabbitmq.sh"]
CMD ["./build/okx_orderbook"] 