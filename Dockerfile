# syntax=docker/dockerfile:1.7

FROM ubuntu:24.04 AS builder

ARG DEBIAN_FRONTEND=noninteractive
ARG BUILD_JOBS

RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential \
      ca-certificates \
      cmake \
      dpkg-dev \
      git \
      libarchive-dev \
      libboost-dev \
      libgrpc++-dev \
      libgrpc-dev \
      libprotobuf-dev \
      protobuf-compiler \
      protobuf-compiler-grpc \
      python3-dev \
    && rm -rf /var/lib/apt/lists/*

COPY linuxcnc-patches/base-revision linuxcnc-patches/apply.sh linuxcnc-patches/*.patch /tmp/linuxcnc-patches/

RUN git init /src/linuxcnc \
    && git -C /src/linuxcnc remote add origin https://github.com/LinuxCNC/linuxcnc.git \
    && git -c http.version=HTTP/1.1 -C /src/linuxcnc fetch --depth=1 origin "$(cat /tmp/linuxcnc-patches/base-revision)" \
    && git -C /src/linuxcnc checkout --detach FETCH_HEAD \
    && /tmp/linuxcnc-patches/apply.sh --detach /src/linuxcnc

RUN cd /src/linuxcnc \
    && export DEB_BUILD_PROFILES="pkg.linuxcnc.headless nodoc" \
    && ./debian/configure no-docs \
    && apt-get update \
    && apt-get -P "pkg.linuxcnc.headless,nodoc" build-dep -y --no-install-recommends . \
    && rm -rf /var/lib/apt/lists/*

RUN cd /src/linuxcnc/src \
    && build_jobs="${BUILD_JOBS:-$(nproc)}" \
    && ./autogen.sh \
    && ./configure \
      --prefix=/opt/linuxcnc \
      --with-realtime=uspace \
      --enable-headless \
      --disable-build-documentation \
      --disable-build-manpages \
    && make -j"${build_jobs}" \
    && make DESTDIR=/linuxcnc-root install

WORKDIR /src/linuxcnc-ctrl
COPY CMakeLists.txt ./CMakeLists.txt
COPY native ./native
COPY proto ./proto

RUN build_jobs="${BUILD_JOBS:-$(nproc)}" \
    && cmake -S . -B /build/native-release \
      -DLINUXCNC_ROOT=/src/linuxcnc \
      -DLINUXCNC_GRPC_BUILD_WIRE=ON \
      -DLINUXCNC_GRPC_BUILD_TESTS=OFF \
      -DLINUXCNC_GRPC_ENABLE_NML=ON \
      -DCMAKE_BUILD_TYPE=MinSizeRel \
    && cmake --build /build/native-release --parallel "${build_jobs}" \
      --target linuxcnc-grpc-server linuxcnc-grpc-health-check \
    && cmake --install /build/native-release --prefix /usr/local --strip \
    && cmake -S . -B /build/native-test \
      -DLINUXCNC_ROOT=/src/linuxcnc \
      -DLINUXCNC_GRPC_BUILD_WIRE=ON \
      -DLINUXCNC_GRPC_BUILD_TESTS=ON \
      -DLINUXCNC_GRPC_ENABLE_NML=ON \
      -DCMAKE_BUILD_TYPE=Debug \
    && cmake --build /build/native-test --parallel "${build_jobs}" \
      --target linuxcnc-grpc-live-integration

FROM ubuntu:24.04 AS runtime

ARG DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
      libarchive13t64 \
      libboost-python1.83.0 \
      libedit2 \
      libfmt9 \
      libgrpc++1.51t64 \
      libmodbus5 \
      libgpiod2t64 \
      libpython3.12t64 \
      libprotobuf32t64 \
      libtirpc3t64 \
      libudev1 \
      libusb-1.0-0 \
      procps \
      psmisc \
      python3 \
      python3-numpy \
      tcl8.6 \
      tclreadline \
      tini \
    && rm -rf /var/lib/apt/lists/*

COPY --from=builder /linuxcnc-root/ /
COPY --from=builder /usr/local/bin/linuxcnc-grpc-server /usr/local/bin/
COPY --from=builder /usr/local/bin/linuxcnc-grpc-health-check /usr/local/bin/
COPY --chmod=0755 docker/linuxcnc-grpc-display /usr/local/bin/linuxcnc-grpc-display
COPY --chmod=0755 docker/linuxcnc-simulator-entrypoint /usr/local/bin/linuxcnc-simulator-entrypoint
COPY docker/linuxcnc.ld.so.conf /etc/ld.so.conf.d/linuxcnc.conf

ENV PATH="/opt/linuxcnc/bin:${PATH}" \
    LD_LIBRARY_PATH="/opt/linuxcnc/lib" \
    PYTHONPATH="/opt/linuxcnc/lib/python3.12/site-packages" \
    LINUXCNC_INI="/config/linuxcnc.ini" \
    LINUXCNC_UID="1000" \
    LINUXCNC_GID="1000" \
    LINUXCNC_GRPC_ENDPOINT="0.0.0.0:50051" \
    LINUXCNC_TELEMETRY_ENDPOINT="0.0.0.0:50052" \
    LINUXCNC_GRPC_WORKSPACE_ROOT="/var/lib/linuxcnc-grpc/workspaces"

RUN ldconfig && mkdir -p /config /var/lib/linuxcnc-grpc/workspaces

EXPOSE 50051 50052
VOLUME ["/config", "/var/lib/linuxcnc-grpc"]
HEALTHCHECK --interval=5s --timeout=3s --start-period=30s --retries=6 \
  CMD ["linuxcnc-grpc-health-check", "127.0.0.1:50051"]

ENTRYPOINT ["/usr/bin/tini", "-g", "--", "/usr/local/bin/linuxcnc-simulator-entrypoint"]

FROM runtime AS test

COPY --from=builder /build/native-test/native/server/linuxcnc-grpc-live-integration /usr/local/bin/
COPY native/server/tests/fixtures/simple_linear.ngc /opt/linuxcnc-simulator/test/simple_linear.ngc
