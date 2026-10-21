#!/usr/bin/env bash
#
# Usage:
#   ./setup_tunnel.sh <user>@<host>
#   ./setup_tunnel.sh <user>@<host> <local-udp-port> <local-tcp-port> \
#                                  <remote-tcp-port> <remote-udp-port>
#
# Defaults to a single-machine layout if only the host argument is given:
#   local-udp-port  = 3000   (sender binds here)
#   local-tcp-port  = 5000   (local udp2tcp talks to this)
#   remote-tcp-port = 4000   (remote tcp2udp listens here)
#   remote-udp-port = 3000   (receiver binds here)
#
# Assumes `udp2tcp` and `tcp2udp` are installed on the respective machines
# (see https://github.com/mullvad/udp-over-tcp), and that passwordless or
# key-based SSH is configured.

set -euo pipefail

if [ $# -lt 1 ]; then
  echo "usage: $0 <user>@<host> [local-udp local-tcp remote-tcp remote-udp]" >&2
  exit 1
fi

REMOTE="$1"
LOCAL_UDP_PORT="${2:-3000}"
LOCAL_TCP_PORT="${3:-5000}"
REMOTE_TCP_PORT="${4:-4000}"
REMOTE_UDP_PORT="${5:-3000}"

LOCAL_PROXY_PID=""
SSH_PID=""

cleanup () {
  if [ -n "${SSH_PID}" ]; then
    kill "${SSH_PID}" 2>/dev/null || true
    wait "${SSH_PID}" 2>/dev/null || true
  fi
  if [ -n "${LOCAL_PROXY_PID}" ]; then
    kill "${LOCAL_PROXY_PID}" 2>/dev/null || true
    wait "${LOCAL_PROXY_PID}" 2>/dev/null || true
  fi
}
trap cleanup EXIT INT TERM

echo "[1/3] Starting local udp2tcp: ${LOCAL_UDP_PORT}/UDP -> ${LOCAL_TCP_PORT}/TCP"
udp2tcp --udp-listen "127.0.0.1:${LOCAL_UDP_PORT}" \
        --tcp-forward "127.0.0.1:${LOCAL_TCP_PORT}" &
LOCAL_PROXY_PID=$!
sleep 0.3
if ! kill -0 "${LOCAL_PROXY_PID}" 2>/dev/null; then
  echo "udp2tcp failed to start" >&2
  exit 1
fi

echo "[2/3] Opening SSH tunnel: 127.0.0.1:${LOCAL_TCP_PORT} -> ${REMOTE}:${REMOTE_TCP_PORT}"
ssh -N \
    -o ExitOnForwardFailure=yes \
    -o ServerAliveInterval=15 \
    -L "127.0.0.1:${LOCAL_TCP_PORT}:127.0.0.1:${REMOTE_TCP_PORT}" \
    "${REMOTE}" &
SSH_PID=$!
sleep 1
if ! kill -0 "${SSH_PID}" 2>/dev/null; then
  echo "SSH tunnel failed to start" >&2
  exit 1
fi

echo "[3/3] Starting remote tcp2udp: ${REMOTE_TCP_PORT}/TCP -> ${REMOTE_UDP_PORT}/UDP"
ssh "${REMOTE}" \
    "tcp2udp --tcp-listen 127.0.0.1:${REMOTE_TCP_PORT} \
             --udp-forward 127.0.0.1:${REMOTE_UDP_PORT}" &
REMOTE_PROXY_SSH_PID=$!

echo
echo "Tunnel ready."
echo "  - On the RECEIVER, run:  ./udp_transfer --receive"
echo "  - On the SENDER,   run:  ./udp_transfer"
echo "  - In the GUI, set Target IP to 127.0.0.1 and target port to ${LOCAL_UDP_PORT}"
echo
echo "Press Ctrl-C to tear down the tunnel."

# Wait for the remote proxy's SSH session to exit, or Ctrl-C.
wait "${REMOTE_PROXY_SSH_PID}"
