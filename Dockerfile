# --- Stage 1: Builder ---
FROM alpine:3.21 AS builder
RUN apk add --no-cache \
    build-base \
    cmake \
    ninja \
    git \
    sqlite-dev \
    nlohmann-json \
    curl-dev \
    curl-static \
    zlib-dev \
    openssl-dev \
    libpsl-dev \
    libidn2-dev \
    nghttp2-dev \
    libssh2-dev \
    brotli-dev \
    zstd-dev

WORKDIR /src
COPY CMakeLists.txt ./
COPY include ./include
COPY src ./src
RUN cmake -S . -B build \
    -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCURL_USE_STATIC_LIBS=ON \
    && cmake --build build --parallel "$(nproc)"

# --- Stage 2: Runtime ---
FROM alpine:3.21
RUN apk add --no-cache \
    sqlite-libs \
    libstdc++ \
    libgcc \
    libssh2 \
    openssl \
    zlib \
    brotli-libs \
    zstd-libs \
    libidn2 \
    nghttp2 \
    libpsl

WORKDIR /app
COPY --from=builder /src/build/ddci /app/ddci
COPY .ddci_vibe /app/.ddci_vibe
RUN chown root:root /app/ddci /app/.ddci_vibe && chmod +x /app/ddci

CMD ["/app/ddci"]