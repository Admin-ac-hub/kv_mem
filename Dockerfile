FROM ubuntu:24.04 AS builder

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
       build-essential \
       cmake \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

RUN cmake -S . -B build -DBUILD_TESTING=OFF -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build --parallel --target kv_stress

FROM ubuntu:24.04

WORKDIR /app
COPY --from=builder /src/build/kv_stress ./build/kv_stress
COPY scripts/stress_test.sh ./scripts/stress_test.sh

ENV SKIP_BUILD=1

ENTRYPOINT ["./scripts/stress_test.sh"]
