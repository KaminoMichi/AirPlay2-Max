#!/system/bin/sh
set -eu
. "$(dirname "$0")/common.sh"
require_root
if "$HELPER" status; then exit 0; else rc=$?; fi
if [ -r "$RUN/status.json" ]; then
 echo 'Last recorded session (may have already exited):'
 cat "$RUN/status.json"
fi
exit "$rc"
