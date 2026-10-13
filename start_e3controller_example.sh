#!/bin/sh
# Launch the E3Controller.
#
# All configuration now lives in a YAML file — `--config` is the only argument.
# The 20 CLI options this script used to pass are gone; see
# configs/e3_controller.yaml for the schema, and include/e3_config.h for why the
# radio geometry belongs in one place.
#
# usage: ./start_e3controller_example.sh [config.yaml]

cd "$(dirname "$0")" || exit 1

CONFIG="${1:-configs/e3_controller.yaml}"

if [ ! -f "$CONFIG" ]; then
    echo "config not found: $CONFIG" >&2
    echo "usage: $0 [config.yaml]" >&2
    exit 2
fi

# Reminders for things this file no longer sets, because they are in the YAML:
#
#   - encoding: `e3.encoding` (asn1 | json). Note the port convention differs —
#     a JSON/cuBB dApp expects 5555/5556/5557, ASN.1 uses 9990/9991/9999. Both
#     are in the `e3:` section, so keep one config file per encoding rather than
#     trying to override them here.
#   - radio geometry: `radio:` — must match the running gNB. The controller
#     validates it against the first slot the RAN delivers and refuses on a
#     mismatch, so a wrong value is a startup error rather than a wrong spectrum.
#   - cbf16 scale: `shm.cbf16_scale`, formerly the E3_CBF16_SCALE env var. It is
#     now pushed down to the gNB-side publish helper, so setting the old env var
#     has no effect.

echo "=== E3Controller: config=$CONFIG ==="
exec ./e3_controller --config "$CONFIG"
