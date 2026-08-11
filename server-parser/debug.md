# Debugging Guide

This guide describes a practical workflow for debugging the C
server/parser application that combines:

-   TCP server/client connections
-   Multiple serial ports
-   `poll()`-based I/O multiplexing
-   Radar data streaming
-   CLI/configuration traffic
-   Logging
-   `libgpiod` hardware watchdog
-   Signal-based shutdown
-   Dynamic memory and buffers

The goal is to make debugging reproducible, starting with simple runtime
checks and progressing to GDB, sanitizers, Valgrind, system-call
tracing, and network/serial inspection.

------------------------------------------------------------------------

# 1. Build a Debug Version

Always keep a debug build available.

Compile with debug symbols and low optimization:

``` bash
clang -g3 -O0 -Wall -Wextra -Wpedantic \
    -o build/server_parser \
    src/*.c \
    -lgpiod
```

If the project uses a Makefile:

``` bash
make clean
make CFLAGS="-g3 -O0 -Wall -Wextra -Wpedantic"
```

Useful compiler options:

``` text
-g3       Maximum useful debug information
-O0       Disable optimization while debugging
-Wall     Common warnings
-Wextra   Additional warnings
-Wpedantic Strict ISO C diagnostics
```

For debugging, avoid starting with `-O2` or `-O3`. Optimizations can
make variables disappear or cause execution to differ from the source
code.

------------------------------------------------------------------------

# 2. Check Compiler Warnings First

Before running the program, make sure the compiler reports no unexpected
warnings.

``` bash
clang -g3 -O0 -Wall -Wextra -Wpedantic \
    -o build/server_parser \
    src/*.c \
    -lgpiod
```

Pay particular attention to warnings involving:

-   incompatible pointer types
-   signed/unsigned comparisons
-   implicit conversions
-   uninitialized variables
-   incorrect format strings
-   missing return values
-   incorrect function declarations
-   buffer sizes
-   use-after-free patterns
-   integer truncation

For example:

``` c
printf("%zu\n", buffer_size);
```

is preferable to using an incompatible format specifier for `size_t`.

------------------------------------------------------------------------

# 3. Run the Application Manually

First run the application without a debugger.

``` bash
./build/server_parser /dev/pts/3 /dev/pts/4
```

Check:

-   Does the process start?
-   Are both serial ports opened successfully?
-   Is the TCP socket created?
-   Is `bind()` successful?
-   Is `listen()` successful?
-   Does `poll()` start?
-   Are client connections accepted?
-   Does serial data arrive?
-   Does serial data get forwarded to TCP?
-   Does TCP data reach the correct serial port?
-   Does SIGINT shut the application down cleanly?

Stop with:

``` text
Ctrl+C
```

------------------------------------------------------------------------

# 4. Create Virtual Serial Ports

For development without radar hardware, use `socat`:

``` bash
sudo apt install socat
```

Create two connected pseudo-terminal pairs:

``` bash
socat -d -d pty,raw,echo=0 pty,raw,echo=0
```

Example:

``` text
PTY is /dev/pts/3
PTY is /dev/pts/4
```

Keep this terminal running.

Then start the application:

``` bash
./build/server_parser /dev/pts/3 /dev/pts/4
```

The important distinction is:

``` text
Application CLI port   -> one PTY endpoint
Application radar port -> another PTY endpoint
```

------------------------------------------------------------------------

# 5. Test Serial Input Independently

Send test data into the radar-side PTY:

``` bash
echo "RADAR_TARGET_DATA_TEST" > /dev/pts/4
```

Or use binary data:

``` bash
printf '\x01\x02\x03\x04\x05' > /dev/pts/4
```

To inspect data received from the other endpoint:

``` bash
cat /dev/pts/3
```

For more controlled testing:

``` bash
dd if=/dev/zero bs=1024 count=1 > /dev/pts/4
```

For continuous traffic:

``` bash
while true; do
    printf 'RADAR_TARGET_DATA_TEST\n' > /dev/pts/4
    sleep 0.1
done
```

Stop it with:

``` text
Ctrl+C
```

------------------------------------------------------------------------

# 6. Verify Serial Device Configuration

For real serial devices, inspect the configuration:

``` bash
stty -F /dev/ttyUSB0 -a
stty -F /dev/ttyUSB1 -a
```

Check the expected baud rates:

``` bash
stty -F /dev/ttyUSB0 921600 raw -echo
stty -F /dev/ttyUSB1 115200 raw -echo
```

Check which process has a serial device open:

``` bash
lsof /dev/ttyUSB0
lsof /dev/ttyUSB1
```

Or:

``` bash
fuser -v /dev/ttyUSB0
fuser -v /dev/ttyUSB1
```

If `open()` fails, verify permissions:

``` bash
ls -l /dev/ttyUSB0 /dev/ttyUSB1
groups
```

On Ubuntu, the user normally needs to be in the `dialout` group:

``` bash
sudo usermod -aG dialout "$USER"
```

Log out and back in after changing group membership.

------------------------------------------------------------------------

# 7. Debug with GDB

Start GDB:

``` bash
gdb ./build/server_parser
```

Run the application with arguments:

``` gdb
run /dev/pts/3 /dev/pts/4
```

If the application crashes, inspect the backtrace:

``` gdb
bt
```

For a more detailed backtrace:

``` gdb
bt full
```

Inspect the current frame:

``` gdb
frame 0
```

Show source:

``` gdb
list
```

Inspect variables:

``` gdb
print variable_name
```

Inspect a pointer:

``` gdb
print pointer
print *pointer
```

Inspect memory:

``` gdb
x/32bx buffer
```

Useful memory formats:

``` text
x/32bx address    32 bytes hexadecimal
x/16wx address    16 words
x/32cb address    32 characters
x/s address       C string
```

------------------------------------------------------------------------

# 8. Set Breakpoints

Break on important functions:

``` gdb
break main
break serial_read
break serial_write
break tcp_accept
break tcp_send
break tcp_receive
```

If the actual function names differ, use:

``` gdb
info functions
```

Break on system calls:

``` gdb
break poll
break accept
break recv
break send
break read
break write
```

Conditional breakpoint:

``` gdb
break parser.c:150 if bytes_received > 1024
```

Continue:

``` gdb
continue
```

Step over:

``` gdb
next
```

Step into:

``` gdb
step
```

Finish the current function:

``` gdb
finish
```

------------------------------------------------------------------------

# 9. Debug the `poll()` Loop

Because the server uses `poll()`, inspect the descriptor array
carefully.

For example:

``` gdb
print nfds
print pollfds[0]
print pollfds[1]
```

Inspect individual fields:

``` gdb
print pollfds[0].fd
print pollfds[0].events
print pollfds[0].revents
```

The most important field is:

``` c
revents
```

Common values include:

``` text
POLLIN    data available
POLLOUT   ready for writing
POLLERR   error
POLLHUP   hangup
POLLNVAL  invalid file descriptor
```

If the program repeatedly wakes up without receiving useful data, check
for:

``` c
POLLHUP
POLLERR
POLLNVAL
```

A common bug is leaving a closed descriptor inside the `pollfd` array.

------------------------------------------------------------------------

# 10. Check File Descriptor Lifetime

For a server with several serial and TCP descriptors, keep track of
ownership.

Typical descriptors:

``` text
serial CLI
serial radar
TCP listening socket
TCP client 1
TCP client 2
...
```

When closing a descriptor:

``` c
close(fd);
```

make sure it is also removed from the active `pollfd` array.

A useful debugging print is:

``` c
printf("closing fd=%d\n", fd);
```

Also log every successful `open()`, `socket()`, `accept()`, and
`close()`.

Example:

``` c
printf("serial fd=%d opened\n", fd);
printf("client fd=%d connected\n", client_fd);
printf("client fd=%d closed\n", client_fd);
```

This makes descriptor leaks and accidental descriptor reuse much easier
to find.

------------------------------------------------------------------------

# 11. Debug TCP Connections

Check whether the server is listening:

``` bash
ss -ltnp
```

For a specific port:

``` bash
ss -ltnp | grep <PORT>
```

Test the server with `nc`:

``` bash
nc 127.0.0.1 <PORT>
```

If the server expects binary data:

``` bash
printf '\x01\x02\x03\x04' | nc 127.0.0.1 <PORT>
```

For interactive TCP testing:

``` bash
nc -v 127.0.0.1 <PORT>
```

Check established connections:

``` bash
ss -tnp
```

Check which process owns the socket:

``` bash
lsof -iTCP:<PORT> -n -P
```

------------------------------------------------------------------------

# 12. Capture TCP Traffic

Use `tcpdump`:

``` bash
sudo tcpdump -i lo -nn -X tcp port <PORT>
```

For a network interface:

``` bash
sudo tcpdump -i eth0 -nn -X tcp port <PORT>
```

Useful options:

``` text
-nn   Do not resolve names or ports
-X    Show packet contents
-v    Verbose output
```

Save a capture for Wireshark:

``` bash
sudo tcpdump -i eth0 -nn -w server.pcap tcp port <PORT>
```

Open it later with Wireshark.

For localhost traffic, use:

``` bash
sudo tcpdump -i lo -nn -X tcp port <PORT>
```

------------------------------------------------------------------------

# 13. Use `strace` for System-Call Debugging

`strace` is especially useful for this application because it exposes
the actual Linux I/O operations.

Run:

``` bash
strace -f -o strace.log ./build/server_parser /dev/pts/3 /dev/pts/4
```

Inspect the log:

``` bash
less strace.log
```

Trace only important I/O and networking calls:

``` bash
strace -f \
    -e trace=openat,close,read,write,poll,ppoll,recvfrom,sendto,recvmsg,sendmsg,accept,accept4,connect,socket,bind,listen \
    ./build/server_parser /dev/pts/3 /dev/pts/4
```

This is useful for questions such as:

``` text
Did open() succeed?
What fd was returned?
Did poll() wake up?
What caused poll() to wake up?
Did read() return zero?
Did write() fail?
Did accept() fail?
Did send()/recv() return an error?
```

------------------------------------------------------------------------

# 14. Always Check `errno`

When a system call fails, immediately inspect `errno`.

Example:

``` c
ssize_t n = read(fd, buffer, sizeof(buffer));

if (n < 0) {
    perror("read");
}
```

Prefer:

``` c
fprintf(stderr, "read(fd=%d) failed: %s\n",
        fd, strerror(errno));
```

For networking:

``` c
if (send(fd, data, len, 0) < 0) {
    perror("send");
}
```

Do not print `errno` long after the failing system call because another
library/system call may have changed it.

------------------------------------------------------------------------

# 15. AddressSanitizer

AddressSanitizer is usually the first memory-debugging tool to try.

Build with:

``` bash
clang -g3 -O1 -fno-omit-frame-pointer \
    -fsanitize=address \
    -Wall -Wextra \
    -o build/server_parser_asan \
    src/*.c \
    -lgpiod
```

Run:

``` bash
./build/server_parser_asan /dev/pts/3 /dev/pts/4
```

ASan can detect problems such as:

-   heap buffer overflow
-   stack buffer overflow
-   use-after-free
-   double free
-   invalid memory access
-   some memory leaks

For leak detection:

``` bash
ASAN_OPTIONS=detect_leaks=1 \
./build/server_parser_asan /dev/pts/3 /dev/pts/4
```

------------------------------------------------------------------------

# 16. UndefinedBehaviorSanitizer

Build with:

``` bash
clang -g3 -O1 -fno-omit-frame-pointer \
    -fsanitize=undefined \
    -Wall -Wextra \
    -o build/server_parser_ubsan \
    src/*.c \
    -lgpiod
```

Run:

``` bash
./build/server_parser_ubsan /dev/pts/3 /dev/pts/4
```

UBSan can detect issues such as:

-   signed integer overflow
-   invalid shifts
-   invalid enum values
-   misaligned access
-   invalid pointer conversions
-   certain integer conversion problems

For more aggressive debugging, combine sanitizers:

``` bash
clang -g3 -O1 -fno-omit-frame-pointer \
    -fsanitize=address,undefined \
    -Wall -Wextra \
    -o build/server_parser_san \
    src/*.c \
    -lgpiod
```

------------------------------------------------------------------------

# 17. Valgrind

Run the server with the requested full memory check:

``` bash
valgrind --leak-check=full --track-origins=yes --show-leak-kinds=all ./build/server_parser
```

If the program requires serial-port arguments:

``` bash
valgrind --leak-check=full \
    --track-origins=yes \
    --show-leak-kinds=all \
    ./build/server_parser /dev/pts/3 /dev/pts/4
```

For more useful output:

``` bash
valgrind \
    --leak-check=full \
    --track-origins=yes \
    --show-leak-kinds=all \
    --num-callers=30 \
    --log-file=valgrind.log \
    ./build/server_parser /dev/pts/3 /dev/pts/4
```

Then:

``` bash
less valgrind.log
```

Look for:

``` text
Invalid read
Invalid write
Use after free
Invalid free
Conditional jump or move depends on uninitialised value
definitely lost
indirectly lost
possibly lost
still reachable
```

The most important leak category is:

``` text
definitely lost
```

because it normally indicates memory that can no longer be reached by
the program.

------------------------------------------------------------------------

# 18. Debug Memory Ownership

For every dynamically allocated object, identify:

``` text
Who allocates it?
Who owns it?
Who modifies it?
Who frees it?
What happens if an error occurs?
```

Example:

``` c
buffer = malloc(size);
```

Immediately document its ownership.

A useful pattern is:

``` c
void *buffer = malloc(size);

if (buffer == NULL) {
    perror("malloc");
    return -1;
}
```

At shutdown:

``` c
free(buffer);
buffer = NULL;
```

Setting pointers to `NULL` after freeing is not required for correctness
by itself, but can make accidental reuse easier to identify.

------------------------------------------------------------------------

# 19. Debug Buffer Boundaries

The radar data path is particularly sensitive to buffer overruns.

Before every copy, verify:

``` text
destination capacity >= bytes being copied
```

For example:

``` c
if (len > sizeof(buffer)) {
    fprintf(stderr, "buffer overflow prevented: len=%zu\n", len);
    return -1;
}

memcpy(buffer, data, len);
```

For accumulated data:

``` c
if (accumulated + received > BUFFER_SIZE) {
    fprintf(stderr, "accumulation buffer full\n");
    ...
}
```

Do not assume that a single `read()` or `recv()` call returns the entire
protocol packet.

TCP is a byte stream.

For example:

``` text
send():
    1000 bytes

recv():
    300 bytes
    500 bytes
    200 bytes
```

The application must handle this correctly.

------------------------------------------------------------------------

# 20. Debug Serial Read Sizes

Serial reads can also return partial data.

Log:

``` c
printf("serial fd=%d read %zd bytes\n", fd, n);
```

For binary data, print hexadecimal:

``` c
for (ssize_t i = 0; i < n; i++) {
    printf("%02X ", buffer[i]);
}

printf("\n");
```

For large radar streams, avoid printing every byte because logging
itself can become the bottleneck.

Instead print:

``` text
timestamp
fd
number of bytes
buffer occupancy
packet count
error state
```

Example:

``` text
RADAR fd=7 bytes=1024 accumulated=8192 packets=32
```

------------------------------------------------------------------------

# 21. Check for Partial TCP Writes

Do not assume:

``` c
send(fd, buffer, len, 0);
```

always sends `len` bytes.

The return value can be smaller than `len`.

Correct logic conceptually is:

``` text
remaining = len

while remaining > 0:
    n = send(...)
    if n < 0:
        handle error
    remaining -= n
```

This is especially important when forwarding high-rate radar data.

------------------------------------------------------------------------

# 22. Check for TCP Disconnects

For `recv()`:

``` c
ssize_t n = recv(fd, buffer, sizeof(buffer), 0);

if (n == 0) {
    /* Peer closed connection */
}

if (n < 0) {
    /* Error */
}
```

A return value of:

``` text
0
```

does not mean "no data right now".

For a TCP socket it normally means:

``` text
The peer performed an orderly shutdown.
```

Remove the client from the `poll()` set and close its descriptor.

------------------------------------------------------------------------

# 23. Handle `EINTR` Correctly

Signals can interrupt system calls.

For example:

``` c
ssize_t n = read(fd, buffer, size);

if (n < 0 && errno == EINTR) {
    /* Retry or return to the main event loop */
}
```

The same concept applies to:

``` text
poll()
read()
write()
accept()
recv()
send()
```

This is particularly important because the application uses `SIGINT` for
shutdown.

------------------------------------------------------------------------

# 24. Debug Signal Handling

Do not perform complex operations inside a signal handler.

A safe pattern is:

``` c
static volatile sig_atomic_t running = 1;

static void handle_sigint(int sig)
{
    (void)sig;
    running = 0;
}
```

Then the main loop checks:

``` c
while (running) {
    ...
}
```

Install the handler:

``` c
signal(SIGINT, handle_sigint);
```

For more robust applications, `sigaction()` is generally preferable.

Test:

``` text
Ctrl+C
```

Then verify that:

-   `poll()` exits or is interrupted
-   client sockets are closed
-   serial descriptors are closed
-   watchdog resources are released
-   allocated memory is freed
-   the process exits with the expected status

------------------------------------------------------------------------

# 25. Enable Core Dumps

For crashes outside GDB, enable core dumps:

``` bash
ulimit -c unlimited
```

Run the program:

``` bash
./build/server_parser /dev/pts/3 /dev/pts/4
```

If it crashes, check:

``` bash
ls -lh core*
```

On systems using systemd:

``` bash
coredumpctl list
```

Inspect a crash:

``` bash
coredumpctl info
```

Open it with GDB:

``` bash
coredumpctl debug
```

Then:

``` gdb
bt full
```

------------------------------------------------------------------------

# 26. Debug Watchdog Problems

The watchdog uses `libgpiod` and must be considered separately from the
TCP/serial logic.

If the application unexpectedly resets or exits, determine whether:

``` text
the main loop is blocked
serial reception stops
TCP handling blocks
poll() is stuck
watchdog_feed() stops executing
```

Add low-frequency watchdog diagnostics:

``` text
watchdog started
watchdog feed
watchdog stopped
```

Do not print on every watchdog feed if it occurs frequently.

A useful diagnostic is to record:

``` text
last successful watchdog feed timestamp
last serial RX timestamp
last TCP activity timestamp
```

This helps distinguish an application deadlock from a hardware/watchdog
problem.

------------------------------------------------------------------------

# 27. Debug Deadlocks and Hangs

If the application appears frozen, attach GDB:

``` bash
gdb -p $(pidof server_parser)
```

Then:

``` gdb
thread apply all bt
```

For a single-threaded application, inspect:

``` gdb
bt
```

Determine whether it is blocked in:

``` text
poll()
read()
write()
send()
recv()
accept()
```

If the process is stuck in a blocking operation that should be
non-blocking, inspect the file descriptor configuration.

------------------------------------------------------------------------

# 28. Check CPU and Memory Usage

Find the process:

``` bash
pgrep -a server_parser
```

Monitor it:

``` bash
top -p $(pidof server_parser)
```

Or:

``` bash
htop
```

Check memory:

``` bash
ps -o pid,ppid,%cpu,%mem,rss,vsz,stat,cmd -p $(pidof server_parser)
```

For high-rate radar traffic, monitor CPU usage carefully.

Excessive logging can itself cause:

``` text
high CPU usage
large latency
buffer buildup
packet loss
watchdog starvation
```

------------------------------------------------------------------------

# 29. Debug Logging

A useful log format should include:

``` text
timestamp
log level
component
file descriptor
event
result/error
```

Example:

``` text
[2026-08-11 15:20:01.123] INFO  TCP     fd=8 client connected
[2026-08-11 15:20:01.124] INFO  SERIAL  fd=7 rx=1024 bytes
[2026-08-11 15:20:01.125] DEBUG TCP     fd=8 tx=1024 bytes
[2026-08-11 15:20:01.126] ERROR SERIAL  fd=7 read failed: Input/output error
```

Avoid logging every byte in production.

Prefer counters and periodic statistics:

``` text
serial_rx_bytes
serial_tx_bytes
tcp_rx_bytes
tcp_tx_bytes
packets_received
packets_forwarded
dropped_packets
buffer_overruns
connection_count
```

------------------------------------------------------------------------

# 30. Recommended Debugging Order

When something is broken, follow this order rather than immediately
using GDB.

## Step 1 --- Compiler

``` bash
clang -g3 -O0 -Wall -Wextra -Wpedantic ...
```

Fix warnings.

## Step 2 --- Reproduce

Run with virtual serial ports:

``` bash
socat -d -d pty,raw,echo=0 pty,raw,echo=0
```

## Step 3 --- Check system resources

``` bash
lsof
ss
fuser
```

## Step 4 --- Trace system calls

``` bash
strace -f -o strace.log ./build/server_parser ...
```

## Step 5 --- AddressSanitizer

``` bash
./build/server_parser_asan ...
```

## Step 6 --- UndefinedBehaviorSanitizer

``` bash
./build/server_parser_ubsan ...
```

## Step 7 --- Valgrind

``` bash
valgrind --leak-check=full --track-origins=yes --show-leak-kinds=all ./build/server_parser
```

## Step 8 --- GDB

``` bash
gdb ./build/server_parser
```

## Step 9 --- Network capture

``` bash
tcpdump
```

## Step 10 --- Hardware

Only after the software path works reliably with virtual serial ports,
move to:

``` text
Raspberry Pi
    |
    +-- /dev/ttyUSB0
    +-- /dev/ttyUSB1
    |
    +-- radar hardware
```

This separates software bugs from hardware/driver/USB problems.

------------------------------------------------------------------------

# 31. Common Failure Matrix

  -----------------------------------------------------------------------
  Symptom                             First checks
  ----------------------------------- -----------------------------------
  Program crashes                     GDB + ASan

  Segmentation fault                  `bt full` + ASan

  Memory leak                         Valgrind

  Random corrupted data               ASan + buffer boundary checks

  Garbage serial data                 `stty` + baud rate + hex dump

  Serial port cannot open             `ls -l`, `groups`, `lsof`

  TCP client cannot connect           `ss -ltnp`

  TCP connection immediately closes   `recv()` return value + `strace`

  Data disappears                     Partial `read()`/`recv()`/`send()`
                                      handling

  CPU is very high                    logging + `top`/`htop`

  Application hangs                   GDB + `bt`

  `poll()` spins at 100% CPU          inspect `revents`, especially
                                      `POLLHUP/POLLERR/POLLNVAL`

  Watchdog resets system              verify main-loop progress and
                                      watchdog feed

  Crash only with optimization        UB/uninitialized memory; run
                                      ASan/UBSan

  Works with `printf`, fails without  timing/race/undefined behavior
  it                                  likely

  Works with real hardware but not    PTY/termios behavior differs;
  PTYs                                inspect serial configuration

  Works locally but not remotely      `ss` + `tcpdump` + firewall/routing
  -----------------------------------------------------------------------

------------------------------------------------------------------------

# 32. Minimal Debug Session

A typical development session can be:

### Terminal 1 --- Virtual serial ports

``` bash
socat -d -d pty,raw,echo=0 pty,raw,echo=0
```

Assume:

``` text
/dev/pts/3
/dev/pts/4
```

### Terminal 2 --- Server

``` bash
./build/server_parser /dev/pts/3 /dev/pts/4
```

### Terminal 3 --- Generate radar traffic

``` bash
while true; do
    printf 'RADAR_TARGET_DATA_TEST\n' > /dev/pts/4
    sleep 0.1
done
```

### Terminal 4 --- TCP client

``` bash
nc 127.0.0.1 <PORT>
```

Then test:

``` text
serial -> server -> TCP
TCP -> server -> serial
```

If the behavior is incorrect:

``` bash
strace -f -o strace.log ./build/server_parser /dev/pts/3 /dev/pts/4
```

Then:

``` bash
valgrind \
    --leak-check=full \
    --track-origins=yes \
    --show-leak-kinds=all \
    ./build/server_parser /dev/pts/3 /dev/pts/4
```

Finally use:

``` bash
gdb ./build/server_parser
```

------------------------------------------------------------------------

# 33. Debug Checklist

Before considering a bug fixed, verify:

-   [ ] Compiler produces no unexpected warnings
-   [ ] Serial descriptors are opened correctly
-   [ ] Serial baud rates are correct
-   [ ] TCP socket binds successfully
-   [ ] TCP clients can connect
-   [ ] `poll()` reports the expected descriptors
-   [ ] `POLLERR`, `POLLHUP`, and `POLLNVAL` are handled
-   [ ] Partial serial reads are handled
-   [ ] Partial TCP reads are handled
-   [ ] Partial TCP writes are handled
-   [ ] TCP disconnects are handled
-   [ ] `errno` is checked after failed system calls
-   [ ] SIGINT causes a clean shutdown
-   [ ] All descriptors are closed
-   [ ] All allocated memory is released
-   [ ] ASan reports no errors
-   [ ] UBSan reports no errors
-   [ ] Valgrind reports no definite memory leaks
-   [ ] No unexpected CPU/memory growth occurs
-   [ ] Watchdog continues to be serviced
-   [ ] High-rate radar traffic does not overflow buffers
-   [ ] The application behaves correctly without debug logging

------------------------------------------------------------------------

# 34. Best Practice for This Project

For this particular server architecture, debug the application in
layers:

``` text
                 ┌───────────────────────┐
                 │       Application     │
                 │                       │
                 │  parser / routing    │
                 └───────────┬───────────┘
                             │
             ┌───────────────┴───────────────┐
             │                               │
       ┌─────▼─────┐                   ┌─────▼─────┐
       │  Serial   │                   │    TCP    │
       │    I/O    │                   │    I/O    │
       └─────┬─────┘                   └─────┬─────┘
             │                               │
       ┌─────▼─────┐                   ┌─────▼─────┐
       │   PTY /   │                   │   nc /    │
       │ ttyUSB    │                   │ tcpdump   │
       └───────────┘                   └───────────┘
```

First prove the serial path.

Then prove the TCP path.

Then prove the routing logic.

Then test the complete serial-to-TCP and TCP-to-serial paths.

Finally introduce real radar traffic and the hardware watchdog.

This approach makes it much easier to identify whether a failure is
caused by:

``` text
Linux / device
    ↓
serial configuration
    ↓
poll/event handling
    ↓
buffer management
    ↓
parser
    ↓
TCP transport
    ↓
remote client
```

rather than debugging the entire system as one large problem.
