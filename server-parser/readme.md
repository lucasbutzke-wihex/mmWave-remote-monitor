# Project Setup & Overview

This README provides the installation steps and a technical overview of the main C application script, which acts as a protocol engine server managing multiple serial ports, TCP networking, logging, and a hardware watchdog utilizing libgpiod.

```
    sudo apt install clang
```

# Install libgpiod (v2.3.1)

The application utilizes libgpiod version 2 for hardware watchdog support. Build and install it from source using the following commands:

```
    sudo apt install autoconf-archive libtool pkg-config autoconf automake build-essential

    git clone https://github.com/brgl/libgpiod.git

    cd libgpiod
    
    git checkout v2.3.1 

    sudo apt install -y meson ninja-build

    cd ~/libgpiod-2.3.1/
    meson setup build --prefix=/usr/local -Dtools=enabled
    ninja -C build
    sudo ninja -C build install
    sudo ldconfig
```
Main Application Architecture

The main C program acts as a multi-channel protocol engine server. Below is a breakdown of its core components based on the provided script:

- Hardware Watchdog: Initialized via watchdog_start(&wdt, 21, 1.0); on GPIO pin 21 with a 1-second timeout. It is continuously fed during serial data reception (watchdog_feed).

- Serial Interfaces:

    /dev/ttyUSB1 (B115200) — Dedicated to CLI responses and asynchronous packet wrapping.

    /dev/ttyUSB0 (B921600) — Dedicated to high-speed radar data streaming, logging, and buffer accumulation.

- TCP Server: Listens on port TCP_SERVER_PORT for incoming client connections, managing data routing asynchronously.

- Multiplexing (poll): Efficiently monitors serial file descriptors, the TCP listener socket, and active client connections concurrently with a 100ms timeout.

- Safety & Alignment: Uses strict ARM alignment directives (alignas(16)) for the data accumulation buffers and handles graceful shutdowns via SIGINT.