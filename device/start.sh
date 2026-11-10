#!/system/bin/sh
set -eu
. "$(dirname "$0")/common.sh"
require_root
if [ "${1:-}" != --locked ]; then exec "$HELPER" locked start "$@"; fi
shift
[ "$#" -le 1 ] || exit 64
bind=${1:-0.0.0.0}
command -v timeout >/dev/null 2>&1 || { echo "timeout command required" >&2; exit 69; }
"$ROOT/bin/ya-boot-state" prepare
if "$HELPER" status >/dev/null; then echo "Already running" >&2; exit 75; else rc=$?; [ "$rc" = 3 ] || exit "$rc"; fi
"$ROOT/prune-logs.sh"
"$HELPER" preflight "$bind"
# Never take over an existing RC1/YA AirPlay companion or an unknown bridge.
if "$HELPER" nsd-present; then echo 'Existing NSD bridge; stop its owning RC first' >&2; exit 75; else rc=$?; [ "$rc" = 3 ] || exit "$rc"; fi
uid=$(timeout 5 cmd package list packages -U ya.station.airplay | sed -n 's/^package:ya\.station\.airplay uid:\([0-9][0-9]*\).*$/\1/p')
case "$uid" in ''|*[!0-9]*) echo 'Companion UID unavailable' >&2; exit 69;; esac
[ "$uid" -ge 10000 ] || exit 69
export YA_NSD_UID="$uid"
sed "s/@BIND_ADDRESS@/$bind/" "$ROOT/etc/shairport.conf.in" > "$RUN/shairport.conf.tmp"
mv "$RUN/shairport.conf.tmp" "$RUN/shairport.conf"
service_started=0
start_ok=0
finish_start() {
 rc=$?
 trap - EXIT INT TERM HUP
 if [ "$start_ok" = 0 ] && [ "$service_started" = 1 ]; then
  if "$HELPER" stop; then
   timeout 5 am stopservice -n ya.station.airplay/.AirplayNsdService >> "$RUN/companion-start.txt" 2>&1 || :
   timeout 5 am force-stop ya.station.airplay >> "$RUN/companion-start.txt" 2>&1 || :
  else
   echo 'Cleanup incomplete; supervisor/resources preserved. Read run/status.json' >&2
  fi
 fi
 exit "$rc"
}
trap finish_start EXIT
trap 'exit 130' INT
trap 'exit 143' TERM HUP
service_started=1
printf "YA AirPlay owns companion startup\n" > "$RUN/companion.claim"
timeout 5 am start-foreground-service -n ya.station.airplay/.AirplayNsdService > "$RUN/companion-start.txt" 2>&1
n=0
until "$HELPER" bridge "$uid"; do
 n=$((n+1)); [ "$n" -lt 50 ] || { echo 'NSD peer was not ready/verified' >&2; exit 69; }; sleep 0.1
done
# Repeat fail-closed checks immediately before the native supervisor owns children.
"$HELPER" preflight "$bind"
"$HELPER" start
"$HELPER" status
start_ok=1
if ! "$ROOT/voice-start.sh"; then echo "Голосовой мост недоступен; AirPlay оставлен работать. См. run/voice.txt." >&2; fi
