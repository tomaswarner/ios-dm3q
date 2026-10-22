#!/bin/bash
# Flash a boot image onto the running phone with no hands: reboot it into Samsung Download Mode over
# USB (sm_reboot -> xnu 0127 kern.sweet_reboot=download), flash BOOT with samloader, then wait for iOS's
# USB network. The phone must be up with sweetrun reachable at 192.168.7.2, or already in Download Mode.
#
#   tools/phone-flash.sh shim/out/dm3q-xNNg-boot.img
#
# Still manual on DM3Q: the USB cable replug after boot (the eUSB2 repeater hides the soft reconnect).
set -u
IMG=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
PORT=$(cd "$(dirname "$0")/.." && pwd)
ROOT=$(cd "$PORT/.." && pwd)
[ -f "$IMG" ] || { echo "no image $IMG"; exit 1; }
if ping -c1 -t2 192.168.7.2 >/dev/null 2>&1; then
	echo "phone up: rebooting it into Download Mode"
	python3 "$PORT/tools/sweetrun.py" -f "$PORT/tools/sweettest/sm_reboot" sm_reboot download &
	SR=$!
	sleep 8
	kill $SR 2>/dev/null   # the phone resets mid-call: sweetrun never gets its exit line
fi
for i in $(seq 1 40); do
	out=$("$ROOT/tools/samloader-rs/samloader" flash -p BOOT "$IMG" 2>&1 | tail -1)
	if echo "$out" | grep -q "Rebooting device"; then
		echo "flashed $(basename "$IMG") (try $i)"
		break
	fi
	sleep 5
done
echo "$out" | grep -q "Rebooting device" || { echo "flash failed: $out"; exit 1; }
for i in $(seq 1 150); do
	if ping -c1 -t2 192.168.7.2 >/dev/null 2>&1; then
		echo "phone up after ~$((i * 2)) s"
		exit 0
	fi
done
echo "phone not on USB yet (replug the cable)"
exit 2
