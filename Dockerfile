FROM ubuntu:latest AS builder

ARG VCPKG_COMMIT=88a643813a35111a9e8f4c08eeb2b897467dd6df

RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    bash \
    build-essential \
    ca-certificates \
    cmake \
    curl \
    git \
    ninja-build \
    perl \
    pkg-config \
    tar \
    unzip \
    zip \
    && rm -rf /var/lib/apt/lists/*

RUN git clone https://github.com/microsoft/vcpkg.git /opt/vcpkg \
    && git -C /opt/vcpkg checkout "${VCPKG_COMMIT}" \
    && /opt/vcpkg/bootstrap-vcpkg.sh -disableMetrics

WORKDIR /src

COPY vcpkg.json vcpkg-configuration.json ./
RUN /opt/vcpkg/vcpkg install --triplet x64-linux

COPY . .

RUN cmake -S . -B /build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE=/opt/vcpkg/scripts/buildsystems/vcpkg.cmake \
    -DVCPKG_TARGET_TRIPLET=x64-linux \
    && cmake --build /build --target traidle

FROM ubuntu:latest AS runtime

RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    ca-certificates \
    libstdc++6 \
    && rm -rf /var/lib/apt/lists/* \
    && groupadd --system traidle \
    && useradd --system --gid traidle --home-dir /app --create-home traidle \
    && mkdir -p /app/data \
    && chown -R traidle:traidle /app

WORKDIR /app/data
COPY --from=builder /build/traidle /usr/local/bin/traidle
COPY --from=builder /src/web /app/web

USER traidle
ENTRYPOINT ["/usr/local/bin/traidle"]