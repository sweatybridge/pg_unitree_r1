ARG PG_MAJOR=18
FROM postgres:${PG_MAJOR}-bookworm AS build-env
ARG PG_MAJOR

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
       g++ make postgresql-server-dev-${PG_MAJOR} \
    && rm -rf /var/lib/apt/lists/*

FROM build-env AS extension

WORKDIR /src
COPY . /src

RUN make -C pg_unitree_r1 clean \
    && make -C pg_unitree_r1 core-test \
    && make -C pg_unitree_r1 -j"$(nproc)" \
    && make -C pg_unitree_r1 install
