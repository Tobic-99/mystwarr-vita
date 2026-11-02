#!/bin/sh
# Run-time-guided discovery loop: run the desktop build with scripted input until it reaches no untranslated
# code. Each miss is appended to tools/m68k_learned.txt, then discovery/translation/build run again (incremental).
#   tools/learn.sh FRAMES "MW_INPUT spec"
cd "$(dirname "$0")/.." || exit 1
frames=${1:-3000}; input=$2
for i in $(seq 1 40); do
    MW_LEARN=$PWD MW_INPUT="$input" build/cmake/mw_desktop "$frames" > build/learn_run.log 2>&1
    rc=$?
    if [ $rc -ne 3 ]; then echo "learn: run finished (exit $rc) after $i round(s)"; tail -3 build/learn_run.log; exit $rc; fi
    tail -1 build/learn_run.log
    python3 tools/m68k_discover.py > /dev/null && python3 tools/m68krec.py | head -1 || exit 1
    cmake -S . -B build/cmake > /dev/null && ninja -C build/cmake -j4 > build/ninja.log 2>&1 || { tail -5 build/ninja.log; exit 1; }
done
