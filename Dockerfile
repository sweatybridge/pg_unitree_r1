ARG PG_MAJOR=18
FROM postgres:${PG_MAJOR}-bookworm AS build-env
ARG PG_MAJOR

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
       ca-certificates cmake g++ git make postgresql-server-dev-${PG_MAJOR} \
    && rm -rf /var/lib/apt/lists/*

FROM build-env AS extension

WORKDIR /src
COPY . /src

RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build --parallel "$(nproc)" \
    && ctest --test-dir build --output-on-failure \
    && cmake --install build
