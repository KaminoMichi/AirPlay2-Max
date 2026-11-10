#!/system/bin/sh
set -eu
umask 077
R=/data/local/tmp/ya-airplay
[ "$(id -u)" = 0 ] || exit 77
[ -f "$R/voice.enabled" ] && [ ! -L "$R/voice.enabled" ] || exit 0
"$R/voice-stop.sh" --stale-only
p=$(pidof com.yandex.io.sdk); case "$p" in ''|*[!0-9]*) echo 'Voice capability unavailable: SDK process not unique' >&2; exit 69;; esac
s=$(cat "/proc/$p/stat"); set -f; set -- ${s##*) }; [ "$#" -ge 20 ]; shift 19; start=$1
case "$start" in ''|*[!0-9]*) exit 65;; esac
[ ! -L "$R/run/voice.stop" ] && [ ! -L "$R/run/voice.txt" ] || exit 65
[ ! -e "$R/run/voice.stop" ] || rm "$R/run/voice.stop"
"$R/bin/ya-voice" --detach-repeat "$p" "$start" > "$R/run/voice.txt" 2>&1
