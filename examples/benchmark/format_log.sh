#!/bin/bash
if [ -f "$1" ]; then
	cat $1 | sed -n 's/^\(\(Starting VM using\)\|\(Average:\)\) \([0-9]\+\).*$/\4/gp' | sed '/^[0-9]\+$/ {N;s/^\([0-9]\+\)\n\(\([0-9]\)\+\)/\1\t\2/}' | sed '1s/^/Memory\tRuntime\n/' > formatted_"$1"
fi
