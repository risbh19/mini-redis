FROM debian:bookworm-slim AS build
RUN apt-get update && apt-get install -y --no-install-recommends \
    g++ cmake make ca-certificates && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY CMakeLists.txt ./
COPY src ./src
COPY bench ./bench
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF && \
    cmake --build build -j 2

FROM debian:bookworm-slim AS runtime
RUN apt-get update && apt-get install -y --no-install-recommends libstdc++6 && \
    rm -rf /var/lib/apt/lists/* && \
    groupadd --gid 10001 miniredis && useradd --uid 10001 --gid 10001 --no-create-home miniredis && \
    mkdir /data && chown miniredis:miniredis /data
COPY --from=build /src/build/mini-redis /usr/local/bin/mini-redis
USER 10001:10001
WORKDIR /data
VOLUME ["/data"]
EXPOSE 6380
STOPSIGNAL SIGTERM
ENTRYPOINT ["mini-redis"]
CMD ["--bind", "0.0.0.0", "--dir", "/data", "--port", "6380"]
