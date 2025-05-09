FROM ubuntu:24.04


ARG DEBIAN_FRONTEND=noninteractive

RUN apt update && \
    apt-get install \
    libgstreamer1.0-dev \
    libgstreamer-plugins-base1.0-dev \
    libgstreamer-plugins-bad1.0-dev \
    gstreamer1.0-plugins-base \
    gstreamer1.0-plugins-good \
    gstreamer1.0-plugins-bad \
    gstreamer1.0-plugins-ugly \
    gstreamer1.0-libav \
    gstreamer1.0-tools \
    gstreamer1.0-x \
    gstreamer1.0-alsa \
    gstreamer1.0-gl \
    gstreamer1.0-gtk3 \
    gstreamer1.0-qt5 \
    gstreamer1.0-pulseaudio \
    libjson-c-dev \
    libgirepository1.0-dev \
    meson \
    ninja-build \
    libcurl4-openssl-dev \
    -y

WORKDIR /builder

# RUN cd gst-gemini-plugin && \
#     meson setup build --prefix=/usr && \
#     ninja -C builddir

COPY ./gst-gemini-plugin/entrypoint.sh /usr/local/bin/entrypoint.sh
RUN chmod +x /usr/local/bin/entrypoint.sh

ENTRYPOINT [ "/usr/local/bin/entrypoint.sh" ]

CMD ["build"]