# Builds v86 (libv86.mjs + v86.wasm) from a source tree mounted at /src: rust wasm32 target, clang for the C
# parts, Java for Closure Compiler, Node for the code generators.
FROM rust:1.88-bookworm
RUN rustup target add wasm32-unknown-unknown && \
    apt-get update && apt-get install -y --no-install-recommends clang lld make default-jre-headless wget nodejs npm && \
    rm -rf /var/lib/apt/lists/*
WORKDIR /src
