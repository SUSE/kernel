#!/bin/bash
# SPDX-License-Identifier: GPL-2.0

. "$(cd "$(dirname "$0")" && pwd)"/test_common.sh
TID="stress_03"
ERR_CODE=0

ublk_io_and_remove()
{
	run_io_and_remove "$@"
	ERR_CODE=$?
	if [ ${ERR_CODE} -ne 0 ]; then
		echo "$TID failure: $*"
		_show_result $TID $ERR_CODE
	fi
}

if ! _have_program fio; then
	exit "$UBLK_SKIP_CODE"
fi

if ! _have_feature "ZERO_COPY"; then
	exit "$UBLK_SKIP_CODE"
fi

_prep_test "stress" "run IO and remove device(zero copy)"

_create_backfile 0 256M
_create_backfile 1 128M
_create_backfile 2 128M

ublk_io_and_remove 8G -t null -q 4 -z &
ublk_io_and_remove 256M -t loop -q 4 -z "${UBLK_BACKFILES[0]}" &
ublk_io_and_remove 256M -t stripe -q 4 -z "${UBLK_BACKFILES[1]}" "${UBLK_BACKFILES[2]}" &
wait

if _have_feature "AUTO_BUF_REG"; then
	ublk_io_and_remove 8G -t null -q 4 --auto_zc &
	ublk_io_and_remove 256M -t loop -q 4 --auto_zc "${UBLK_BACKFILES[0]}" &
	ublk_io_and_remove 256M -t stripe -q 4 --auto_zc "${UBLK_BACKFILES[1]}" "${UBLK_BACKFILES[2]}" &
	ublk_io_and_remove 8G -t null -q 4 -z --auto_zc --auto_zc_fallback &
	wait
fi

if _have_feature "PER_IO_DAEMON"; then
	ublk_io_and_remove 8G -t null -q 4 --auto_zc --nthreads 8 --per_io_tasks &
	ublk_io_and_remove 256M -t loop -q 4 --auto_zc --nthreads 8 --per_io_tasks "${UBLK_BACKFILES[0]}" &
	ublk_io_and_remove 256M -t stripe -q 4 --auto_zc --nthreads 8 --per_io_tasks "${UBLK_BACKFILES[1]}" "${UBLK_BACKFILES[2]}" &
	ublk_io_and_remove 8G -t null -q 4 -z --auto_zc --auto_zc_fallback --nthreads 8 --per_io_tasks &
	wait
fi

_cleanup_test "stress"
_show_result $TID $ERR_CODE
