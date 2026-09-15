# run-gate.sh — run one native gate and, if it dies, say where. Sourced by the Linux job in
# .github/workflows/native.yml. Phase N Stage 3.1.
#
# WHY THIS EXISTS AS A FILE: these runners are the ONLY place the Linux and Windows builds ever
# execute, so a crash here has to be diagnosable from the log alone — there is no machine to
# reproduce it on. Two things get in the way by default and both have already cost a round-trip:
#
#   1. `binary 2>&1 | tee log` under `set -e` reports the PIPELINE's status, i.e. tee's, so a
#      segfault looks like success and surfaces several steps later as a missing assertion.
#   2. `local rc=$?` written after an `if` reads the compound statement's status (0 when the
#      condition was false and there is no else branch), not the binary's.
#
# So: no pipe, status captured directly, and a signal death (rc > 128) re-runs the same command
# under gdb for a backtrace in the same job. That backtrace is what turned "the 256z run exits
# silently" into "a data race between the render thread's autorelease pool and the world-load
# thread" in one iteration.
#
# usage:  run_gate <label> <command> [args...]

run_gate() {
  local label="$1"; shift
  # printf, not echo: `tr -c` maps echo's trailing newline to a dash too, so the file lands as
  # `gate-geometry-64z-.log` and the caller's `gate-geometry-64z.log` does not exist.
  local log="gate-$(printf '%s' "$label" | tr -c 'A-Za-z0-9' '-').log"
  local rc=0

  "$@" > "$log" 2>&1 || rc=$?
  if [ "$rc" -eq 0 ]; then
    echo "::group::$label — passed ($(wc -l < "$log") lines of output)"
    tail -n 20 "$log"
    echo "::endgroup::"
    return 0
  fi

  echo "::group::$label — FAILED, exit $rc — tail of $log"
  tail -n 40 "$log"
  echo "::endgroup::"

  if [ "$rc" -gt 128 ] && command -v gdb >/dev/null 2>&1; then
    echo "::group::$label — backtrace"
    gdb -q -batch -ex run -ex "bt 40" -ex "info threads" --args "$@" 2>&1 | tail -n 80
    echo "::endgroup::"
  fi
  return "$rc"
}
