# E3Manager

A standalone C++ daemon that connects to srsRAN's jbpf shared memory (IPC primary) to receive and process codelet output data. Fully self-contained — builds the jbpf IO library from source.

## Prerequisites

- CMake ≥ 3.16
- GCC/G++ with C++17 support
- pkg-config
- yaml-cpp (`libyaml-cpp-dev`)
- Boost (`libboost-dev`, `libboost-program-options-dev`, `libboost-filesystem-dev`)

## Build

```bash
# Clone with submodules
git clone --recurse-submodules <repo-url>
cd E3Manager

# Initialize jbpf submodules (only needed once)
cd jbpf && bash ./init_and_patch_submodules.sh && cd ..

# Build
mkdir -p build && cd build
cmake .. -DINITIALIZE_SUBMODULES=OFF
make -j$(nproc) e3_manager
```

The binary is output to `out/bin/e3_manager`.

## Usage

> **Important:** E3Manager (IPC primary) must start **before** srsRAN (IPC secondary).

```bash
./out/bin/e3_manager [options]
```

### Options

| Option | Default | Description |
|---|---|---|
| `--ipc-name <name>` | `e3_manager` | IPC shared memory segment name |
| `--mem-size <bytes>` | `1073741824` (1GB) | Shared memory size |
| `--poll-interval <us>` | `100` | Poll interval in microseconds |
| `--help` | | Show help |

### Example

```bash
# Terminal 1: Start E3Manager
./out/bin/e3_manager --ipc-name e3_manager

# Terminal 2: Start srsRAN with jbpf agent pointing to "e3_manager"
# Then load the ecpri_iq_samples codelet
```

## Configuration

### Stream IDs

Each codelet output channel has a unique `stream_id` (16-byte identifier). The stream ID in `register_handlers()` ([src/e3_manager.cpp](src/e3_manager.cpp)) must match the one assigned in your codelet load request.

### Adding New Handlers

1. Create a handler class implementing `E3DataHandler` (see [include/e3_data_handler.h](include/e3_data_handler.h))
2. Define its `stream_id` to match the codelet's output channel
3. Register it in `register_handlers()` in `src/e3_manager.cpp`

## Architecture

```
srsRAN + jbpf (IPC Secondary)  ──shared memory──►  E3Manager (IPC Primary)
                                                        │
   ecpri_iq_collect codelet ──► ringbuf ──►             ├──► EcpriIqHandler
   future codelets          ──► ringbuf ──►             └──► (extensible)
```
