#!/bin/sh
# The call letters the app's FCC list has on a frequency, the way the app hands them to the
# engine (Stations.callsOn): K / W calls of 3-4 letters, "-FM" / "-LP" / "-FM1" endings dropped.
#
#   CALLS="$(sh calls.sh 99.1)" IQFILE=kkgi.cu8 IQSTEP=1 build/apptest 99100000 2 60
HERE=$(cd "$(dirname "$0")" && pwd)
LIST=${LIST:-$HERE/../../app/src/main/assets/stations/fm.txt}
awk -F'|' -v want="@$(echo "$1" | awk '{ printf "%d", $1 * 100 + 0.5 }')" '
    /^@/ { on = ($1 == want); next }
    on { c = $1; sub(/-.*/, "", c); if (c ~ /^[KW][A-Z][A-Z][A-Z]?$/ && !seen[c]++) printf "%s%s", n++ ? " " : "", c }
    END { print "" }' "$LIST"
