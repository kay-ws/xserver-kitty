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
    libsixel-dev \
    libchafa-dev \
    libglib2.0-dev \
    libpixman-1-dev \
    libssl-dev \
    xserver-xorg-dev \
    xkb-data \
    x11-xkb-utils \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build
COPY . .
RUN chmod +x build-xkitty-debian.sh
CMD ["./build-xkitty-debian.sh"]
