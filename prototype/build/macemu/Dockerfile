# Emscripten 4.0.22 plus gmp/mpfr, which Basilisk II's FPU needs. The mpfr build script comes from the pinned
# macemu fork (GPL-2), fetched at build time rather than copied into this repository.
FROM emscripten/emsdk:4.0.22-arm64
RUN apt-get update && apt-get install -y autoconf automake libtool libsdl1.2-dev libsdl2-dev lzip bison pkg-config curl && rm -rf /var/lib/apt/lists/*
ARG MACEMU_COMMIT=f44bedce698ff5bf600185a7b15c904b9c43758a
RUN curl -fsSL https://raw.githubusercontent.com/mihaip/macemu/${MACEMU_COMMIT}/BasiliskII/src/Unix/_em_build_mpfr.sh -o /tmp/_em_build_mpfr.sh && bash /tmp/_em_build_mpfr.sh
