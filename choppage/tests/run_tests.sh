#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# CHOPPAGE host tests: the maths, the platform pieces kept from Felucca / X0X / FoMni, the engine and
# sequencer (frame by frame), the break analysis against the factory breaks, the break loaders
# (Python, the browser's), and the whole firmware app in the simulator (tests/scenarios/*.chp:
# UI, perform keys, steps, slices, generator, recording, MIDI, persistence), with screenshots and
# audio in build/scenarios/.
#   tests/run_tests.sh
set -u
cd "$(dirname "$0")/.."
CC="${CC:-cc}"
FAIL=0
OUT=build/host
mkdir -p "$OUT" build/scenarios
run() {
    name="$1"; shift
    if "$@" > "$OUT/$name.log" 2>&1; then
        echo "  ok   $name"
    else
        echo "  FAIL $name (see $OUT/$name.log)"
        tail -15 "$OUT/$name.log" | sed 's/^/       /'
        FAIL=1
    fi
}
STRICT="-O2 -ffp-contract=off -std=c99 -Wall -Wextra -Wdouble-promotion -Werror -Wno-unused-function"
run fastmath sh -c "$CC -O2 -ffp-contract=off -Wall -Wextra -Werror -o $OUT/fastmath_test tests/host/fastmath_test.c -lm && $OUT/fastmath_test"
run encoder sh -c "$CC -O2 -w -Ifirmware/hal -o $OUT/encoder_test tests/host/encoder_test.c && $OUT/encoder_test"
run uac sh -c "$CC -O2 -w -Ifirmware/src -Ifirmware/hal -o $OUT/uac_test tests/host/uac_test.c -lm && $OUT/uac_test"
run trs sh -c "$CC -O2 -w -Ifirmware/src -Ifirmware/hal -o $OUT/trs_test tests/host/trs_test.c && $OUT/trs_test"
run dsp-warnings sh -c "for u in engine gen ana factory; do $CC $STRICT -c -o $OUT/\$u.o firmware/src/dsp/\$u.c || exit 1; done"
run engine sh -c "$CC $STRICT -o $OUT/engine_test tests/host/engine_test.c firmware/src/dsp/engine.c firmware/src/dsp/gen.c -lm && $OUT/engine_test"
run analysis sh -c "$CC $STRICT -o $OUT/ana_test tests/host/ana_test.c firmware/src/dsp/ana.c firmware/src/dsp/factory.c && $OUT/ana_test $OUT"
run storage sh -c "$CC -O2 -o $OUT/storage_test tests/storage_test.c && $OUT/storage_test"
# the update path, against the firmware package (Felucca's tests; needs ./build.sh)
if [ -f build/choppage.fwsc ]; then
    run ota-entry sh -c "$CC -o $OUT/ota_test tests/ota_test.c && $OUT/ota_test build/choppage.fwsc"
    if [ -z "${AC79_SDK:-}" ] && [ ! -f "$HOME/fw-AC79_AIoT_SDK/cpu/wl82/tools/uboot.boot" ]; then
        echo "  skip update-loader (needs AC79_SDK, as the build)"
    else
    run update-loader sh -c "head -c 100000 build/choppage.bin > $OUT/old_app.bin && \
        AC79_SDK=\"\${AC79_SDK:-$HOME/fw-AC79_AIoT_SDK}\" python3 tools/fm1pkg_make.py $OUT/old_app.bin build/loader/ota.bin $OUT/old.fwsc >/dev/null && \
        $CC -o $OUT/ldr_test tests/ldr_test.c && $OUT/ldr_test $OUT/old.fwsc build/choppage.fwsc"
    fi
    run installer python3 tests/install_test.py
else
    echo "  skip update-path tests (no build/choppage.fwsc: run ./build.sh)"
fi
run host-build sh host/build_host.sh
run loader python3 tests/loader_test.py
run web python3 tests/web_test.py
if node -e "require('playwright')" 2>/dev/null; then
    run web-page node tests/web_page_test.js build/loader/break140.wav
else
    echo "  skip web-page (needs node and Playwright)"
fi
for s in tests/scenarios/*.chp; do
    n=$(basename "$s" .chp)
    mkdir -p "build/scenarios/$n"
    run "scenario-$n" build/host/chp_host "$s" "build/scenarios/$n"
done
[ $FAIL -eq 0 ] && echo "all tests passed" || echo "TESTS FAILED"
exit $FAIL
