# E3Controller

A standalone C++ daemon that bridges ocudu's jbpf shared memory (IPC primary) with the E3 protocol via [libe3](https://github.com/wineslab/libe3). It receives I/Q sample data from jbpf codelets and exposes it as E3 Service Model indications to subscribed dApps.


> To use this E3Controller you need to build and run [this version](https://github.com/wineslab/ocudu-e3) of OCUDU.
> The controller and its codelets are built against that checkout's jbpf, so build the gNB and
> the controller from the same checkout (see [Using OCUDU's jbpf](docs/build.md#using-ocudus-jbpf)).

## Documentation

| Document | Contents |
|---|---|
| [Build](docs/build.md) | `build.sh`, what it does, its options and examples, building the whole system, OCUDU's jbpf, ASN.1 code generation |
| [Usage](docs/usage.md) | Running the controller, an example config, every config key, who writes the shared-memory rows, drop accounting |
| [Architecture](docs/architecture.md) | The data path (the slot pipeline, and the obsolete `ecpri_iq` one), the dispatcher, the key source files |
| [Service models](docs/service-models.md) | RF=1 Spectrum and RF=2 L1-KPM, deviations from NVIDIA's schema, RAN function data, adding a Service Model |
| [Stage records](docs/stage-records.md) | Latency tracing with latrec: the stages, reading a capture, ring sizing, what recording costs |
| [Codelets](codelets/README.md) | Building and verifying the jbpf codelets, the context contract, deployment, extension IDs |
| [Uplink slot codelet](codelets/uplink_slot_samples/README.md) | What the RF=2 slot codelet reads and publishes, its timestamps, measured results |
| [Known limitations](docs/known-limitations.md) | One encoding per process, the PRB blacklist stub, the compile-time slot size, no load-time verification |

The gNB side (build options, the `jbpf:` section the gNB needs) is in ocudu's
[README_JBPF.md](https://github.com/wineslab/ocudu-e3/blob/e3/README_JBPF.md).

## Attribution

The **RF=2 L1-KPM** service model targets NVIDIA Aerial dApps: its
`indicationMessage.protocolData` payload conforms to NVIDIA Aerial's public E3
message schema, in both JSON and ASN.1 (APER).

- JSON (`src/e3sm/l1_kpm/e3sm_layer1_json.cpp`) reproduces NVIDIA's `protocolData`
  keys directly.
- ASN.1 (`src/e3sm/asn/e3sm_layer1.asn`, `L1KPM-Indication`) is wineslab's ASN.1
  representation **derived from** the same JSON schema (NVIDIA publishes the JSON
  schema only).

Source schema (Apache-2.0, `Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES`):
- [e3_message_schemas.json](https://github.com/NVIDIA/aerial-sample-apps/blob/main/dapps/docs/e3_message_schemas.json)
- [e3_message_examples.json](https://github.com/NVIDIA/aerial-sample-apps/blob/main/dapps/docs/e3_message_examples.json)

Where the payload differs from the schema, see
[Service models](docs/service-models.md#l1-kpm-rf2-and-nvidias-schema).

The **RF=1 Spectrum** service model (PRB-blacklist / spectrum sharing) is
wineslab's own and is not part of NVIDIA's schema.

## How To Cite

If you use the E3Controller, please reference the following paper:

>Angelo Feraudo, Ravis Shirkhani, Stefano Maxenti, Andrea Lacava, Michele Polese, and Tommaso Melodia. 2026. 
> <i>Bringing dApps to OCUDU: An E3 Controller for Real-Time Open RAN Intelligence. </i>
>In Proceedings of the 20th ACM Workshop on Wireless Network Testbeds, Experimental evaluation & Characterization (WiNTECH '26). 
> Association for Computing Machinery, New York, NY, USA, 17–24. https://doi.org/10.1145/3831662.3844165
