# E3Manager

A standalone C++ daemon that bridges srsRAN's jbpf shared memory (IPC primary) with the E3 protocol via [spear-libe3](https://github.com/wineslab/spear-libe3). It receives I/Q sample data from jbpf codelets and exposes it as E3 Service Model indications to subscribed dApps.

## Prerequisites

- CMake ≥ 3.16
- GCC/G++ with C++17 support
- pkg-config
- yaml-cpp (`libyaml-cpp-dev`)
- Boost (`libboost-dev`, `libboost-program-options-dev`, `libboost-filesystem-dev`) — needed by jbpf
- [mouse07410/asn1c](https://github.com/mouse07410/asn1c) — ASN.1 compiler with APER support
- [spear-libe3](https://github.com/wineslab/spear-libe3) — installed system-wide (see below)
- ZeroMQ (`libzmq3-dev`)

### Installing asn1c (mouse07410 fork)

```bash
sudo apt-get install -y bison flex
git clone https://github.com/mouse07410/asn1c.git
cd asn1c
test -f configure || autoreconf -iv
./configure
make -j$(nproc)
sudo make install
```

### Installing spear-libe3

```bash
git clone https://github.com/wineslab/spear-libe3.git
cd spear-libe3
mkdir -p build && cd build
cmake ..
make -j$(nproc)
sudo make install
```

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

### ASN.1 Code Generation

The build automatically:
1. Runs `asn1c` on `src/e3sm/asn/e3sm_spectrum.asn` to generate C encoder/decoder code
2. Compiles the generated code into a static library (`e3sm_asn`) linked to `e3_manager`

Generated files go into `build/asn1c_generated/` and are **not** tracked in git.

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

## Architecture

```
srsRAN + jbpf (IPC Secondary)  ──shared memory──►  E3Manager (IPC Primary)
                                                        │
                                                    JbpfDispatcher
                                                   (single poll loop)
                                                    /           \
                                           stream_id_A     stream_id_B
                                                │               │
                                          E3SMSpectrum      (future SM)
                                                │
                                          APER encode ──► emit to dApps
```

### Dispatcher Model

The `JbpfDispatcher` is the central routing component:

1. **Single poll loop** — the main thread calls `dispatcher.poll()` which invokes `jbpf_io_channel_handle_out_bufs` once
2. **Stream routing** — each SM registers its `stream_id` + callback; the dispatcher routes buffers by matching stream_id
3. **Centralized buffer release** — the dispatcher always releases all buffers after the callback returns, preventing leaks
4. **Multi-SM safe** — multiple SMs can coexist without double-free or missed buffers

### Adding a New Service Model

1. Create a class extending `libe3::ServiceModel` (see `E3SMSpectrum` as reference)
2. In `start()`, register your stream_id with the dispatcher:
   ```cpp
   dispatcher_.register_stream(my_stream_id_,
       [this](jbpf_io_stream_id* sid, void** bufs, int n) {
           process_buffers(sid, bufs, n);
       });
   ```
3. In `stop()`, unregister: `dispatcher_.unregister_stream(my_stream_id_);`
4. Add ASN.1 types + encoding wrappers for your SM's messages
5. Register in `e3_manager.cpp`:
   ```cpp
   agent.register_sm(std::make_unique<MyNewSM>(dispatcher));
   ```

> **Note:** Do NOT release buffers in your SM callback — the dispatcher handles that.

### Key Components

| File | Description |
|---|---|
| `src/e3_manager.cpp` | Main daemon — jbpf IO init, E3 agent, dispatcher poll loop |
| `include/jbpf_dispatcher.h` | Central buffer dispatcher — routes by stream_id, releases buffers |
| `src/e3sm_spectrum.cpp` | Spectrum SM — APER-encodes I/Q data and emits to subscribers |
| `include/e3sm_spectrum.h` | Spectrum SM class (extends `libe3::ServiceModel`) |
| `src/e3sm/e3sm_spect_wrapper.h` | C++ wrapper structs for ASN.1 Spectrum types |
| `src/e3sm/e3sm_spect_wrapper.cpp` | APER encoding via asn1c-generated C types |
| `src/e3sm/asn/e3sm_spectrum.asn` | ASN.1 definitions for Spectrum SM messages |
