#!/system/bin/sh
set -eu
. "$(dirname "$0")/common.sh"
require_root
command -v timeout >/dev/null 2>&1 || { echo "timeout command required" >&2; exit 69; }
if [ "${1:-}" != --locked ]; then exec "$HELPER" locked stop "$@"; fi
shift
[ "$#" = 0 ] || exit 64
"$ROOT/bin/ya-boot-state" prepare
"$ROOT/voice-stop.sh"
"$HELPER" stop
# The shared APK may be used by old RC1. Stop it only if THIS RC actually started it.
if [ -f "$RUN/companion.claim" ]; then
 timeout 5 am stopservice -n ya.station.airplay/.AirplayNsdService >> "$RUN/companion-stop.txt" 2>&1 || :
 if ! timeout 5 am force-stop ya.station.airplay >> "$RUN/companion-stop.txt" 2>&1; then
  echo 'Companion force-stop failed; claim/logs preserved' >&2; exit 69
 fi
 n=0
 while "$HELPER" nsd-present; do
  n=$((n+1)); [ "$n" -lt 50 ] || { echo 'Companion socket remains; claim preserved' >&2; exit 69; }; sleep 0.1
 done
 # A failed inspection is not proof of absence.
 if "$HELPER" nsd-present; then exit 69; else rc=$?; [ "$rc" = 3 ] || exit "$rc"; fi
 rm -f "$RUN/companion.claim"
fi
echo 'YA AirPlay children reaped; only its companion claim released'
