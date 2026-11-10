# YA AirPlay: no awk and no signals based on pidfiles.
ROOT=$(CDPATH= cd -P "$(dirname "$0")" && pwd)
case "$ROOT" in /data/local/tmp/ya-airplay) ;; *) echo 'Unexpected install path' >&2; exit 64;; esac
HELPER="$ROOT/bin/ya-supervisor"
RUN="$ROOT/run"
require_root() { [ "$(id -u)" = 0 ] || { echo 'Use existing owner-approved root shell' >&2; exit 77; }; }
