# controller-forward

Low-latency Xbox controller forwarding over the network. Captures physical controller input on one machine and injects it as a virtual Xbox 360 controller on another — similar to how [Moonlight](https://github.com/moonlight-stream/moonlight-qt) / [Sunshine](https://github.com/LizardByte/Sunshine) handle input, but as a standalone tool.

## How it works

```
┌──────────────┐        UDP (12 bytes)       ┌──────────────────┐
│    Sender     │ ─────────────────────────►  │     Receiver     │
│  (XInput)     │    XUSB_REPORT packet       │  (ViGEmBus)      │
│               │                             │                  │
│ Physical Xbox │                             │ Virtual Xbox 360 │
│  controller   │                             │   controller     │
└──────────────┘                              └──────────────────┘
```

**Sender** polls a physical Xbox controller via XInput at ~250 Hz and streams 12-byte `XUSB_REPORT` packets over UDP.

**Receiver** listens for those packets and injects them into Windows as a virtual Xbox 360 controller through the ViGEmBus kernel driver — no DLLs required, communicates directly via `DeviceIoControl`.

The hot path is three syscalls with zero allocations: `recvfrom()` → `memcpy()` → `DeviceIoControl()`.

## Prerequisites

### Receiver machine
- **Windows 10/11**
- **[ViGEmBus driver](https://github.com/nefarius/ViGEmBus/releases)** — install the latest release

### Sender machine
- **Windows 10/11** with an Xbox controller connected

### Build toolchain (either machine)
- **[MinGW-w64](https://winlibs.com/)** (g++) — or any C++17 compiler targeting Windows

## Building

```batch
build.bat
```

Or manually:

```bash
# Receiver
g++ -O2 -Wall -std=c++17 -Ivigem/include -o controller-receiver.exe controller-receiver.cpp -lsetupapi -lws2_32

# Sender
g++ -O2 -Wall -std=c++17 -o controller-sender.exe controller-sender.cpp -lxinput1_4 -lws2_32
```

## Usage

**On the receiver** (the machine where the game runs):

```
controller-receiver.exe [port]
controller-receiver.exe 9876
```

**On the sender** (the machine with the physical controller):

```
controller-sender.exe <receiver-ip> [port] [poll-rate-hz] [controller-index]
controller-sender.exe 192.168.1.50 9876 250 0
```

| Argument           | Default     | Description                          |
|--------------------|-------------|--------------------------------------|
| `receiver-ip`      | `127.0.0.1` | IP address of the receiver machine   |
| `port`             | `9876`      | UDP port                             |
| `poll-rate-hz`     | `250`       | How often to poll the controller (Hz)|
| `controller-index` | `0`         | XInput user index (0-3)              |

## Why UDP?

TCP's reliability guarantees cause **head-of-line blocking** — if one packet is lost, all subsequent packets are held until retransmission completes. For controller input, only the latest state matters. A lost packet is better than a delayed one. This is the same approach used by Moonlight, Parsec, and Steam Remote Play.

## Project structure

```
├── controller-receiver.cpp     # Receiver: UDP → ViGEmBus virtual controller
├── controller-sender.cpp       # Sender: XInput → UDP
├── vigem/
│   └── include/ViGEm/
│       ├── Common.h            # XUSB_REPORT, button definitions
│       └── BusShared.h         # IOCTL codes, driver structures
├── build.bat                   # Build script
├── LICENSE                     # MIT
└── README.md
```

## License

MIT — see [LICENSE](LICENSE).

ViGEm header definitions are derived from [nefarius/ViGEmBus](https://github.com/nefarius/ViGEmBus) (MIT License).

