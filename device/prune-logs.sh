#!/system/bin/sh
# Called under the native operation lock, only after status confirmed absent.
# Retain four prior sessions; start creates the fifth. All entries are validated
# before deletion. Unknown objects cause refusal, never broad recursive removal.
set -eu
R=/data/local/tmp/ya-airplay/run
set -- "$R"/log-*
[ -e "$1" ] || exit 0
for d do
 [ -d "$d" ] && [ ! -L "$d" ] || exit 65
 name=${d##*/}; rest=${name#log-}; stamp=${rest%-*}; pid=${rest#*-}
 case "$stamp:$pid" in *[!0-9:]*|:*|*:) exit 65;; esac
 [ "$(stat -c %u "$d")" = 0 ] || exit 77
 for f in "$d"/* "$d"/.[!.]* "$d"/..?*; do
  [ -e "$f" ] || [ -L "$f" ] || continue
  case "${f##*/}" in receiver.txt|nqptp.txt|supervisor.txt|events.log) ;; *) exit 65;; esac
  [ -f "$f" ] && [ ! -L "$f" ] && [ "$(stat -c %u "$f")" = 0 ] && [ "$(stat -c %h "$f")" = 1 ] || exit 65
 done
done
# Numeric epoch session names are fixed-width for the supported 2026 firmware.
list=$(printf '%s\n' "$@" | sort)
count=$#
while IFS= read -r d; do
 [ "$count" -gt 4 ] || break
 for f in "$d"/* "$d"/.[!.]* "$d"/..?*; do [ ! -f "$f" ] || rm "$f"; done
 rmdir "$d"
 count=$((count-1))
done <<END_LOG_LIST
$list
END_LOG_LIST
