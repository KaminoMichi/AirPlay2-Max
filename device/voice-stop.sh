#!/system/bin/sh
# Only the stop file is a command. PID/start/exe are read-only evidence used
# to retire stale metadata; no signal is ever sent to a recorded PID.
set -eu
umask 077
R=/data/local/tmp/ya-airplay
[ "$(id -u)" = 0 ] || exit 77
mode=${1:-stop}; case "$mode" in stop|--stale-only) ;; *) exit 64;; esac
A="$R/run/voice.active"
[ ! -L "$A" ] && [ ! -L "$R/run/voice.stop" ] || exit 65
[ -e "$A" ] || exit 0
[ -f "$A" ] && [ "$(stat -c %u "$A")" = 0 ] && [ "$(stat -c %h "$A")" = 1 ] || exit 65
identity=$(stat -c '%d:%i' "$A")
IFS=' ' read -r pid started extra < "$A"
case "$pid:$started:$extra" in *[!0-9:]*|:*|*::*|*:*:?) exit 65;; esac
[ -n "$pid" ] && [ -n "$started" ] && [ -z "$extra" ] || exit 65
current() {
 if s=$(cat "/proc/$pid/stat" 2>/dev/null); then
  set -f; set -- ${s##*) }; [ "$#" -ge 20 ] || return 69
  state=$1; shift 19; now=$1
  [ "$now" = "$started" ] || return 3
  case "$state" in Z|X) return 3;; esac
  exe=$(readlink "/proc/$pid/exe") || return 69
  [ "$exe" = "$R/bin/ya-voice" ] || return 69
  return 0
 fi
 [ ! -d "/proc/$pid" ] && return 3
 return 69
}
clear_stale() {
 [ -e "$A" ] || return 0
 [ ! -L "$A" ] && [ "$(stat -c '%d:%i' "$A")" = "$identity" ] || return 65
 rm "$A"
}
if current; then rc=0; else rc=$?; fi
if [ "$rc" = 3 ]; then clear_stale; exit; fi
[ "$rc" = 0 ] || exit "$rc"
[ "$mode" = stop ] || exit 75
[ ! -e "$R/run/voice.stop" ] || exit 75
(set -C; printf 'STOP\n' > "$R/run/voice.stop")
n=0
while [ -e "$A" ]; do
 if current; then :; else rc=$?; [ "$rc" != 3 ] || { clear_stale; exit; }; [ "$rc" = 0 ] || exit "$rc"; fi
 n=$((n+1)); [ "$n" -le 40 ] || exit 69; sleep 0.1
done
