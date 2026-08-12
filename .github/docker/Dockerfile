# syntax=docker/dockerfile:1

########################################
# Stage 1: build libgpiod v2 from source
########################################
FROM ubuntu:24.04 AS gpiod-builder

ARG GPIOD_VERSION=v2.1.3

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        autoconf \
        automake \
        autoconf-archive \
        libtool \
        pkg-config \
        m4 \
        git \
        ca-certificates \
    && rm -rf /var/lib/apt/lists/*

RUN git clone https://git.kernel.org/pub/scm/libs/libgpiod/libgpiod.git /tmp/libgpiod \
    && cd /tmp/libgpiod \
    && git checkout ${GPIOD_VERSION} \
    && ./autogen.sh --enable-tools=yes --prefix=/usr/local \
    && make -j$(nproc) \
    && make DESTDIR=/tmp/gpiod-install install

########################################
# Stage 2: build your project
########################################
FROM ubuntu:24.04 AS builder

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        pkg-config \
    && rm -rf /var/lib/apt/lists/*

# Bring in libgpiod headers + libs built in stage 1
COPY --from=gpiod-builder /tmp/gpiod-install/usr/local /usr/local
RUN ldconfig

WORKDIR /src
COPY . .

RUN make

########################################
# Stage 3: minimal runtime image
########################################
FROM ubuntu:24.04 AS runtime

# Only the shared lib is needed at runtime, not headers/static libs
COPY --from=gpiod-builder /tmp/gpiod-install/usr/local/lib /usr/local/lib
RUN ldconfig

COPY --from=builder /src/build/my_program /usr/local/bin/my_program

ENTRYPOINT ["/usr/local/bin/my_program"]
