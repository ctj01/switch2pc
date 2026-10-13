#!/bin/sh
# min_duration.sh <milliseconds> <command...>: run the command and fail when it
# finishes sooner than that (a paced run cannot be faster than its frames).
min=$1
shift
start=$(date +%s%N)
"$@"
status=$?
end=$(date +%s%N)
elapsed=$(( (end - start) / 1000000 ))
echo "min_duration: elapsed $elapsed ms (minimum $min ms), exit status $status"
if [ "$status" -ne 0 ]; then
    exit "$status"
fi
if [ "$elapsed" -lt "$min" ]; then
    echo "min_duration: too fast"
    exit 1
fi
exit 0
