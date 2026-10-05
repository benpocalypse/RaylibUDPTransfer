#!/usr/bin/env bash
# Run this on the LOCAL machine before starting udp_transfer.
# It assumes udp-over-tcp is installed on both machines
# and that SSH access to the remote host works.

set -e

REMOTE_USER="${1:?usage: setup_tunnel.sh user@host}"
LOCAL_UDP_PORT=3000
LOCAL_TCP_PORT=5000
REMOTE_TCP_PORT=4000
REMOTE_UDP_PORT=6000

echo "Starting local udp->tcp proxy..."
tcp2udp --tcp-listen "localhost:${LOCAL_TCP_PORT}" \
        --udp-forward "localhost:${LOCAL_UDP_PORT}" &
LOCAL_PROXY_PID=$!
trap "kill ${LOCAL_PROXY_PID}" EXIT

echo "Opening SSH tunnel..."
ssh -N -L "${LOCAL_TCP_PORT}:localhost:${REMOTE_TCP_PORT}" "${REMOTE_USER}" &
SSH_PID=$!
trap "kill ${LOCAL_PROXY_PID} ${SSH_PID}" EXIT

echo "Starting remote udp->tcp proxy on ${REMOTE_USER}..."
ssh "${REMOTE_USER}" \
    "udp2tcp --udp-listen 0.0.0.0:${REMOTE_UDP_PORT} \
             --tcp-forward localhost:${REMOTE_TCP_PORT}" &

echo "Tunnel ready. Launch ./udp_transfer and send to 127.0.0.1:${LOCAL_UDP_PORT}"
wait
