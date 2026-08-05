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

# Test Serial Ports

## Create Virtual Serial Ports with socat

Install socat(if not already installed)

```
    sudo apt install socat
```

## Create the Dummy Serial Ports

Instead of writing to restricted paths under /dev, create standard virtual pseudo-terminal pairs in user space. socat will output the actual paths assigned to each end (e.g., /dev/pts/3 and /dev/pts/4):

```
    socat -d -d pty,raw,echo=0 pty,raw,echo=0
```

- How it works:

    Running this command will print out lines indicating the created endpoints, such as:
    Plaintext

    2026/08/05 12:00:00 socat[12345] N PTY is /dev/pts/3
    2026/08/05 12:00:00 socat[12345] N PTY is /dev/pts/4

    Keep this terminal open. You will pass these two distinct pseudo-terminal paths into your application arguments.

- What this does:

    Creates a bidirectional bridge between two virtual serial ports.

    Keeps the terminal open and outputs debugging info as data flows through the ports.


# Run the Updated Main Script with Arguments

Assuming socat allocated /dev/pts/3 for CLI responses and /dev/pts/4 for radar streaming, launch your compiled application by providing the ports as command-line arguments:

```
    ./your_program /dev/pts/3 /dev/pts/4
```

(Where the first argument maps to fd1 / CLI port, and the second argument maps to fd2 / high-speed radar port).

## Test and Interact with the Ports

With socat running, open separate terminal windows to simulate data traffic to and from your application:
Send simulated Radar Data to /dev/pts/4

Your code reads from fd2 (/dev/pts/4), logs it, and pushes it out via TCP:

```
    echo "RADAR_TARGET_DATA_TEST" > /dev/pts/4
```
## Listen to CLI responses from /dev/pts/3

Your code writes responses to fd1 (/dev/pts/3):

```
    cat < /dev/pts/4
```
