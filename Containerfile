FROM debian:bookworm

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    automake \
    autoconf \
    libtool \
    pkg-config \
    xutils-dev \
    xorg-sgml-doctools \
    xfonts-utils \
    libpixman-1-dev \
    libssl-dev \
    xserver-xorg-dev \
    xkb-data \
    x11-xkb-utils \
    zlib1g-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build
COPY . .
RUN chmod +x build-xkitty-debian.sh
CMD ["./build-xkitty-debian.sh"]
