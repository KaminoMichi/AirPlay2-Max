#!/system/bin/sh
# Invoked only by an already installed root manager's /data boot hook.
set -eu
R=/data/local/tmp/ya-airplay
[ "$(id -u)" = 0 ] || exit 77
[ -f "$R/autostart.enabled" ] && [ ! -L "$R/autostart.enabled" ] || exit 0
n=0
while [ "$(getprop sys.boot_completed)" != 1 ]; do
 n=$((n+1)); [ "$n" -le 120 ] || exit 69; sleep 1
done
# One start, no restart storm or guessed-PID signals. Subsequent failures remain
# visible to the existing native supervisor and diagnostic command.
exec /system/bin/sh "$R/start.sh" 0.0.0.0
