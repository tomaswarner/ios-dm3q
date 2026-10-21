#!/bin/bash
# macOS host side of the phone's USB Ethernet (xnu sweetusb=2/3, patches 0075-0077, 0107).
#   tools/sweet-usbnet-mac.sh [LOGFILE]
# Waits for the phone's CDC-ECM interface (host MAC 02:00:5e:77:ee:01, macOS's
# built-in CDC-ECM driver), gives it 192.168.7.1/24 (the phone is 192.168.7.2)
# and prints the live kernel console the phone streams to UDP 5140 (appended
# to LOGFILE if given). Run it in Terminal: the address needs sudo.
set -eu
mac=02:00:5e:77:ee:01
echo "waiting for the phone's USB Ethernet ($mac)..." >&2
ifname=""
until [ -n "$ifname" ]; do
	ifname=$(ifconfig -a | awk -v m="$mac" '/^[a-z]/{i=$1} tolower($0) ~ "ether " m {sub(":$","",i); print i}')
	[ -n "$ifname" ] || sleep 1
done
sudo ifconfig "$ifname" inet 192.168.7.1 netmask 255.255.255.0 up
echo "$ifname up: 192.168.7.1 <-> phone 192.168.7.2; live log on udp/5140 (ping 192.168.7.2)" >&2
exec python3 -u - "${1:-}" <<'EOF'
import socket, sys
log = open(sys.argv[1], "ab") if len(sys.argv) > 1 and sys.argv[1] else None
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("0.0.0.0", 5140))
while True:
    data, _ = s.recvfrom(65536)
    sys.stdout.buffer.write(data); sys.stdout.flush()
    if log:
        log.write(data); log.flush()
EOF
