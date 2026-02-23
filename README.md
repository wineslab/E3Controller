# E3Manager

A standalone C++ daemon that connects to srsRAN's jbpf shared memory (IPC primary) to receive and process codelet output data. Fully self-contained — builds the jbpf IO library and asn1c compiler from source.

## Prerequisites

- CMake ≥ 3.16
- GCC/G++ with C++17 support
- pkg-config
- yaml-cpp (`libyaml-cpp-dev`)
- Boost (`libboost-dev`, `libboost-program-options-dev`, `libboost-filesystem-dev`) (needed for jbpf)
- autoconf, automake, libtool (needed to build asn1c from source)
- git (to clone asn1c fork during build)

## Build

```bash
# Clone with submodules
git clone --recurse-submodules <repo-url>
cd E3Manager

# Initialize jbpf submodules (only needed once)
cd jbpf && bash ./init_and_patch_submodules.sh && cd ..

# Build (asn1c is built from source automatically)
mkdir -p build && cd build
cmake .. -DINITIALIZE_SUBMODULES=OFF
make -j$(nproc) e3_manager

# Note: First build takes longer as it clones and builds the
# mouse07410/asn1c fork (with APER support). Subsequent builds
# skip this step.
```

The binary is output to `out/bin/e3_manager`.

### ASN.1 Code Generation

The build automatically:
1. Clones and builds the [mouse07410/asn1c](https://github.com/mouse07410/asn1c) fork (which supports Aligned PER)
2. Runs `asn1c` on `src/e3sm/asn/e3sm_spectrum.asn` to generate C encoder/decoder code
3. Compiles the generated code into a static library (`e3sm_asn`) linked to `e3_manager`

Generated files go into `build/asn1c_generated/` and are **not** tracked in git.

The ASN.1 definition file (`e3sm_spectrum.asn`) defines the E3 Spectrum Service Model structures:
- `Spectrum-IQDataIndication` — I/Q sample data from RAN (APER-encoded by `EcpriIqHandler`)
- `Spectrum-PRBBlacklistControl` — PRB blacklist control messages
- `Spectrum-ConfigControl` — Spectrum monitoring configuration

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
   ecpri_iq_collect codelet ──► ringbuf ──►            ├──► EcpriIqHandler ──► APER encode
   future codelets          ──► ringbuf ──►            └──► (extensible)
```

