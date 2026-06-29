cd "$(dirname "$0")"

ENCODING="${1:-json}"
case "$ENCODING" in
    json) SETUP_PORT=5555; PUB_PORT=5556; SUB_PORT=5557 ;;
    asn1) SETUP_PORT=9990; PUB_PORT=9991; SUB_PORT=9999 ;;
    *) echo "usage: $0 [json|asn1]" >&2; exit 2 ;;
esac
echo "=== E3Controller: encoding=$ENCODING  ports setup=$SETUP_PORT pub=$PUB_PORT sub=$SUB_PORT ==="

./e3_controller \
    --encoding "$ENCODING" \
    --link-layer zmq \
    --transport tcp \
    --num-prbs <prbs> \
    --shm-name /e3_ran_buffers \
    --shm-size $((1<<30)) \
    --codelet-path /workspace/e3_release/E3Controller/codelets \
    --poll-core <core> \
    --worker-core <core> \
    --publisher-core <core> \
    --setup-port "$SETUP_PORT" \
    --publisher-port "$PUB_PORT" \
    --subscriber-port "$SUB_PORT" \
    --lcm-socket /dev/shm/jbpf/jbpf_lcm_ipc \