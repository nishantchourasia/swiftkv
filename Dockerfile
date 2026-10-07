FROM debian:bookworm-slim AS build
RUN apt-get update && apt-get install -y --no-install-recommends g++ cmake make \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY CMakeLists.txt ./
COPY include ./include
COPY src ./src
COPY apps ./apps
COPY tests ./tests
COPY third_party ./third_party
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build --target swiftkv-server swiftkv-bench --parallel 4

FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends libstdc++6 curl \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --uid 10001 --create-home swiftkv \
    && mkdir /data && chown swiftkv:swiftkv /data
COPY --from=build /src/build/swiftkv-server /src/build/swiftkv-bench /usr/local/bin/
USER swiftkv
WORKDIR /data
EXPOSE 6380 6381
ENTRYPOINT ["stdbuf", "-oL", "swiftkv-server"]
CMD ["--host", "0.0.0.0", "--port", "6380", "--admin-host", "0.0.0.0", "--admin-port", "6381", "--io-threads", "4", "--aof", "/data/appendonly.aof", "--aof-sync", "everysec"]
