#!/usr/bin/env bash
# Smoke test for mod_audio_fork.
#
# What it covers:
#   1. The CMake out-of-tree build produces mod_audio_fork.so against a
#      FreeSWITCH SDK (taken from the voiceagent-fs image as a convenient
#      pre-built environment — any image with the FS SDK + libwebsockets-dev
#      + libspeexdsp-dev + libboost-dev would work).
#   2. The .so contains the expected mod_load entry point.
#   3. The module loads into a transient FS instance and registers the
#      uuid_audio_fork API command.
#   4. The API command's USAGE banner mentions every subcommand we ship.
#   5. Bad inputs (empty args, non-existent UUID) fail gracefully instead
#      of crashing FS.
#
# What it does NOT cover (see protocol_test.sh):
#   - actual audio streaming, playAudio/killAudio/mark protocol behaviour.
#
# Usage:
#   ./tests/smoke.sh

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# The FS container to load the freshly-built module into.
#
#   Default is duck-call's (4th-gen) container; older generations used
#   `voiceagent-fs`. Override with FS_CONTAINER=<name>.
FS_CONTAINER="${FS_CONTAINER:-duckcall-fs}"

# ════════════════════════════════════════════════════════════════════════════
# ★★★ Build against the image you are going to LOAD INTO
# ════════════════════════════════════════════════════════════════════════════
#
# BASE_IMAGE used to default to `voiceagent-fs:latest` (1st generation) while the
# module was installed into `duckcall-fs` (4th generation) — two different
# FreeSWITCH builds with two different `libfreeswitch.so.1`.
#
# ⚠ The failure mode is brutally unhelpful: FreeSWITCH reports
#
#     Error Loading module .../mod_audio_fork.so
#     cannot open shared object file: No such file or directory
#
#   for a file that is plainly present with every NEEDED library resolvable.
#   Hours can go into that message before suspecting the toolchain.
#
# ⇒ Derive the base from the container we install into, so "compiled against"
#   and "loaded into" cannot drift apart. Override BASE_IMAGE to opt out.
BASE_IMAGE="${BASE_IMAGE:-$(docker inspect -f '{{.Config.Image}}' "$FS_CONTAINER" 2>/dev/null)}"
[ -n "$BASE_IMAGE" ] || BASE_IMAGE="voiceagent-fs:latest"
# ★ The builder tag carries the base, so switching bases cannot silently reuse a
#   builder compiled against the other one.
BUILDER_IMAGE="${BUILDER_IMAGE:-mod-audio-fork-builder:$(printf '%s' "$BASE_IMAGE" | tr -c 'A-Za-z0-9_.-' '_')}"
# Where the build lands on the host. `build/` is gitignored.
BUILD_DIR="${BUILD_DIR:-$REPO_ROOT/build}"
MOD_PATH=/usr/local/freeswitch/mod/mod_audio_fork.so

red()   { printf "\033[31m%s\033[0m\n" "$*"; }
green() { printf "\033[32m%s\033[0m\n" "$*"; }
bold()  { printf "\033[1m%s\033[0m\n" "$*"; }

fail()  { red "FAIL: $*"; exit 1; }
pass()  { green "PASS: $*"; }

# ----------------------------------------------------------------------------
# 1. Build via the existing builder image (re-create if missing).
# ----------------------------------------------------------------------------
bold "[1/5] Build mod_audio_fork.so"

if ! docker image inspect "$BUILDER_IMAGE" >/dev/null 2>&1; then
    bold "  builder image missing — creating from $BASE_IMAGE"
    if ! docker image inspect "$BASE_IMAGE" >/dev/null 2>&1; then
        fail "base image $BASE_IMAGE not found — build voiceagent-fs first \
(deploy/fs/build.sh in the voice-agent repo)"
    fi
    # ★ DOCKER_BUILDKIT=0: buildkit's builder may run in its own container and
    #   not share the local image store, so a LOCAL-only base image fails with
    #   "pull access denied, repository does not exist". The legacy builder reads
    #   the local store directly. (Measured: duckcall-fs:spike exists locally and
    #   buildkit still tried to pull it from Docker Hub.)
    DOCKER_BUILDKIT=0 docker build -t "$BUILDER_IMAGE" -f - "$REPO_ROOT" <<EOF >/dev/null
FROM $BASE_IMAGE
USER root
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        cmake build-essential pkg-config \
        libwebsockets-dev libspeexdsp-dev libboost-dev \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
ENTRYPOINT []
CMD ["bash"]
EOF
fi

# Build, size-check, and symbol-check in a single container so /tmp/build
# stays alive across the steps (each `docker run --rm` is otherwise a fresh,
# empty filesystem).
# ★★★ The artifact must land on the HOST, not in the --rm container.
#
#   It used to build into the container's /tmp/build and only extract the size
#   plus the `nm` output — the freshly built .so was destroyed when the
#   container exited. Steps 3-5 then `docker exec`'d into a *running* FS and
#   probed the module baked into THAT image.
#
#   ⇒ Steps 3-5 never tested the code you just built. Measured proof: the
#     container's baked .so was 239,120 bytes dated May 25 while step 1 had
#     just produced 323,008 bytes. Two different binaries, two months apart —
#     and an all-PASS run reads as "my change loads and registers fine".
#
#   Now: build into $BUILD_DIR on the host, install it into the container,
#   and verify by sha256 that the module FS loaded is the one we just built.
mkdir -p "$BUILD_DIR"
COMBINED_OUT=$(docker run --rm -v "$REPO_ROOT:/src" -v "$BUILD_DIR:/out" "$BUILDER_IMAGE" bash -c '
    set -e
    rm -rf /tmp/build
    # ★★★ 编译输出**不许丢**。原来两行都是 >/dev/null 2>&1，于是编译失败时
    #   外层只能打印 `build / introspection failed (output: )` —— 括号里是空的。
    #   ⚠ 实测撞到：PR-3 撞了一个既有函数名，而那条 `conflicting types` 被吞掉了，
    #     我只能另起一个 docker run 才看到它。★ 一个不肯说原因的构建失败
    #     比失败本身更贵。
    cmake -S /src -B /tmp/build > /tmp/cmake_cfg.log 2>&1 \
      || { echo "CMAKE_CONFIG_FAILED"; tail -30 /tmp/cmake_cfg.log; exit 1; }
    cmake --build /tmp/build --parallel > /tmp/cmake_build.log 2>&1 \
      || { echo "CMAKE_BUILD_FAILED"; grep -E "error|Error" /tmp/cmake_build.log | head -20; exit 1; }
    test -f /tmp/build/mod_audio_fork.so || { echo "NO_ARTIFACT"; exit 1; }
    cp /tmp/build/mod_audio_fork.so /out/mod_audio_fork.so
    echo "ARTIFACT_SIZE:$(stat -c%s /out/mod_audio_fork.so)"
    echo "ARTIFACT_SHA:$(sha256sum /out/mod_audio_fork.so | cut -d" " -f1)"
    # ★ Run the ws_uri unit tests HERE, in the same container: the binary is a
    #   Linux ELF and the host may be macOS. Its full output is echoed so a
    #   failure shows which case broke, not just that something did.
    for u in ws_uri_test throttle_test json_escape_test; do
      test -x /tmp/build/$u || { echo "UNIT_MISSING:$u"; exit 1; }
    done
    echo "UNIT_BEGIN"
    unit_rc=0
    for u in ws_uri_test throttle_test json_escape_test; do
      /tmp/build/$u || unit_rc=$?
    done
    echo "UNIT_RC:$unit_rc"
    echo "UNIT_END"
    # FreeSWITCH discovers modules via the module_interface data symbol +
    # mod_load entry point. The interface lives in the data section (D),
    # the entry points in text (T) — accept either.
    nm -D /out/mod_audio_fork.so 2>/dev/null | grep -E " [TD] mod_audio_fork_"
') || fail "build / introspection failed (output: $COMBINED_OUT)"

FRESH_SHA=$(echo "$COMBINED_OUT" | sed -n 's/^ARTIFACT_SHA://p')
[ -n "$FRESH_SHA" ] || fail "no sha256 for the built artifact"

SO_SIZE=$(echo "$COMBINED_OUT" | sed -n 's/^ARTIFACT_SIZE://p')
[ -z "$SO_SIZE" ] && fail "no .so produced"
pass "built mod_audio_fork.so (${SO_SIZE} bytes)"

# ----------------------------------------------------------------------------
# 1b. ws_uri unit tests — the URL parser (PR-1).
# ----------------------------------------------------------------------------
bold "[1b] ws_uri unit tests"

MISSING=$(echo "$COMBINED_OUT" | sed -n 's/^UNIT_MISSING://p')
[ -z "$MISSING" ] || fail "unit test binary '$MISSING' was not built — still in CMakeLists.txt?"
UNIT_RC=$(echo "$COMBINED_OUT" | sed -n 's/^UNIT_RC://p')
UNIT_OUT=$(echo "$COMBINED_OUT" | sed -n '/^UNIT_BEGIN$/,/^UNIT_END$/p' | sed '1d;$d')
[ -n "$UNIT_RC" ] || fail "no ws_uri_test result in the build output"
if [ "$UNIT_RC" != "0" ]; then
    printf '%s\n' "$UNIT_OUT"
    fail "ws_uri_test exited $UNIT_RC"
fi
# ★★ COUNTER-PROOF: a suite that silently ran zero cases must not pass.
#    Each binary self-checks a minimum count too — asserting it here as well
#    means neither side can go vacuous alone. That matters because
#    "0 checks, exit 0" is indistinguishable from "all checks passed" if you
#    only look at the exit code.
#
# ⚠ And every suite must be named here explicitly: a `for` over whatever
#   happened to print would pass when one of them printed nothing at all.
for suite in ws_uri drop_throttle json_escape; do
    line=$(echo "$UNIT_OUT" | grep -oE "==> $suite: [0-9]+ checks passed" | head -1)
    [ -n "$line" ] || fail "no '==> $suite: N checks passed' line — that suite did not run its table
  (exit code was 0, which on its own cannot tell 'all passed' from 'ran nothing')"
    pass "$(echo "$line" | sed 's/==> //')"
done

# ----------------------------------------------------------------------------
# 2. Symbol check — module entry point present.
# ----------------------------------------------------------------------------
bold "[2/5] Inspect .so symbols"

for sym in mod_audio_fork_module_interface mod_audio_fork_load mod_audio_fork_shutdown; do
    echo "$COMBINED_OUT" | grep -qE " [TD] $sym$" || fail "missing exported symbol: $sym"
done
pass "module entry symbols present"

# ----------------------------------------------------------------------------
# 3. Load the module into a transient FS, verify the API is registered.
# ----------------------------------------------------------------------------
bold "[3/5] Load module into transient FS"

if ! docker inspect "$FS_CONTAINER" --format '{{.State.Status}}' 2>/dev/null | grep -q running; then
    fail "FS container '$FS_CONTAINER' not running.
  Running containers: $(docker ps --format '{{.Names}}' | tr '\n' ' ')
  → start one (duck-call: 'make fs-up') or set FS_CONTAINER=<name>.
  (A standalone test container would be cleaner but needs a
   SignalWire-token-built FS image; reusing a running one is pragmatic.)"
fi

# The ESL password comes from the container's own env — hardcoding ClueCon
# only works on a factory-default FS, and a wrong password fails as a wall of
# -ERR whose root cause is invisible.
ESL_PW=$(docker inspect -f '{{range .Config.Env}}{{println .}}{{end}}' "$FS_CONTAINER" \
    | sed -n 's/^FS_ESL_PASSWORD=//p' | head -1)
[ -n "$ESL_PW" ] || ESL_PW="${FS_ESL_PASSWORD:-ClueCon}"
fs() { docker exec "$FS_CONTAINER" /usr/local/freeswitch/bin/fs_cli -p "$ESL_PW" -x "$*" 2>&1; }

# ★★★ Install the freshly built module, then reload it.
#
#   Without this, everything below probes whatever was baked into the image.
docker cp "$BUILD_DIR/mod_audio_fork.so" "$FS_CONTAINER:$MOD_PATH" \
    || fail "could not install the built .so into $FS_CONTAINER"
# ★ Confirm it landed BEFORE restarting. `docker cp` into a running container
#   is not atomic, and a restart that races it produces
#   "cannot open shared object file: No such file or directory" — which reads
#   like a broken build rather than a timing problem. (Measured: hit it once.)
COPIED_SIZE=$(docker exec "$FS_CONTAINER" stat -c%s "$MOD_PATH" 2>/dev/null || echo 0)
[ "$COPIED_SIZE" = "$SO_SIZE" ] || fail "the .so in $FS_CONTAINER is $COPIED_SIZE bytes, built $SO_SIZE
  → docker cp did not complete; do not restart into a half-written module"

# ════════════════════════════════════════════════════════════════════════════
# ★★★ RESTART the container — `reload` is not enough, and it lies
# ════════════════════════════════════════════════════════════════════════════
#
# The first version of this block did `fs "reload mod_audio_fork"` and accepted
# the result if it matched /\+OK|Reload|success/i. Measured output:
#
#     +OK Reloading XML
#     -ERR unloading module [Module in use.]
#
# The grep matched the FIRST line — which is about XML, not about the module —
# while the unload had plainly failed. So:
#
#   · the new .so sat on disk,
#   · the sha256 check below said "yes, that is the file I built",
#   · and FreeSWITCH went on executing the OLD code from memory.
#
# ⇒ Steps 3-5 kept validating a stale module even after this harness was
#   supposedly fixed to prevent exactly that. A file-level sha proves the FILE,
#   never the RUNNING CODE. (Caught by a functional test: the parser rejected a
#   port of 99999 in its unit tests while FS logged "port 99999" happily.)
#
# ★ `unload` fails with "Module in use." whenever any channel holds a media bug,
#   which is most of the time. A container restart is deterministic, costs a few
#   seconds, and removes the entire question.
#   ⚠ It also drops live calls — acceptable for a test container, and a clean
#     slate is what a smoke test wants anyway.
#
# ★★ The real answer to "is FS running my build?" is the module reporting its
#    own identity — that is PR-3 (`audio_fork_version`). That now exists, and
#    step 3b below asserts it against the version the SOURCE declares.
#    The restart stays: the version string proves WHICH BUILD, not that FS
#    reloaded it, and those are different claims.
docker restart "$FS_CONTAINER" >/dev/null || fail "could not restart $FS_CONTAINER"
for _ in $(seq 60); do
    STATE=$(docker inspect -f '{{.State.Health.Status}}' "$FS_CONTAINER" 2>/dev/null || echo "")
    [ "$STATE" = "healthy" ] && break
    # No healthcheck configured? Fall back to "fs_cli answers".
    [ -z "$STATE" ] && fs "status" 2>/dev/null | grep -q "^UP" && break
    sleep 1
done
fs "status" | grep -q "^UP" || fail "$FS_CONTAINER did not come back up after restart"

# ════════════════════════════════════════════════════════════════════════════
# 3b. ★★★ Ask the module who it is, and hold it to what the source says.
# ════════════════════════════════════════════════════════════════════════════
#
# Why this is not redundant with the sha check below:
#
#   sha         proves the FILE on disk is the one we built
#   restart     proves FS re-read that file
#   THIS        proves the RUNNING CODE claims the version we think we shipped
#
# The third one is the only one a CONSUMER can make. tanxun-aicc's deploy runbook
# says "tag is a label, not proof — ask the module itself", and it asks exactly
# this API. So this assertion is the thing that keeps that runbook honest.
#
# ⚠ It caught a real state: 0.2.0..HEAD was five commits — a use-after-destroy
#   mutex, three data races, and a new event — all reporting "0.2.0". Both of the
#   consumer's environments ran the pre-fix build and *no command could tell*,
#   because the only version-bearing string had not moved. A harness that builds
#   the module and never asks its version cannot notice that.
#
# ★ Parsed out of the source, not hardcoded here: a literal in two places is a
#   literal that will disagree, and the failure mode is a green test on a wrong
#   version — the exact class of bug this block exists to catch.
SRC_VERSION=$(sed -n 's/^#define MOD_AUDIO_FORK_VERSION "\(.*\)"/\1/p' "$REPO_ROOT/mod_audio_fork.h")
[ -n "$SRC_VERSION" ] || fail "could not parse MOD_AUDIO_FORK_VERSION out of mod_audio_fork.h"

VER_JSON=$(fs "audio_fork_version")
echo "$VER_JSON" | grep -q '"module":"mod_audio_fork"' \
    || fail "audio_fork_version did not answer as mod_audio_fork — got: $VER_JSON"
echo "$VER_JSON" | grep -q "\"version\":\"$SRC_VERSION\"" || fail \
"the RUNNING module reports a different version than the source declares
  source says:  $SRC_VERSION
  module says:  $VER_JSON
  → either the build is stale, or MOD_AUDIO_FORK_VERSION was not bumped for this release"
pass "running module reports version $SRC_VERSION (matches source)"

# Every capability bit the source defines as 1 must show up as true, and the
# ones defined as 0 must show up as false.
# ★ A bit that lies is worse than a bit that is absent (see mod_audio_fork.h) —
#   a consumer negotiates on these, so a stale `true` makes it skip a workaround
#   it still needs.
for bit in frame_drop_metrics:CAP_FRAME_DROP_METRICS \
           media_silent:CAP_MEDIA_SILENT \
           lockfree_writes:CAP_LOCKFREE_WRITES \
           multithread_safe:CAP_MULTITHREAD_SAFE; do
    json_name=${bit%%:*}; macro=${bit##*:}
    src_val=$(sed -n "s/^#define $macro  *\([01]\).*/\1/p" "$REPO_ROOT/mod_audio_fork.h")
    [ -n "$src_val" ] || fail "could not parse $macro out of mod_audio_fork.h"
    [ "$src_val" = "1" ] && want=true || want=false
    echo "$VER_JSON" | grep -q "\"$json_name\":$want" || fail \
"capability $json_name: source defines $macro=$src_val (=> $want) but the running module disagrees
  $VER_JSON"
done
pass "all capability bits match the source"

EXISTS=$(fs "module_exists mod_audio_fork" | tr -d '[:space:]')
[ "$EXISTS" = "true" ] || fail "module_exists returned '$EXISTS' (expected 'true') after restart
  → check modules.conf.xml actually loads mod_audio_fork"

# ★ COUNTER-PROOF (necessary, not sufficient): the file FS just loaded from
#   must be the one we built. Necessary because a wrong file means the wrong
#   code; not sufficient because a file can match while stale code runs — which
#   is why the restart above is not optional.
IN_CONTAINER_SHA=$(docker exec "$FS_CONTAINER" sha256sum "$MOD_PATH" 2>/dev/null | cut -d' ' -f1)
[ "$IN_CONTAINER_SHA" = "$FRESH_SHA" ] || fail "the module in $FS_CONTAINER is NOT the one just built
  built:        $FRESH_SHA
  in container: $IN_CONTAINER_SHA
  ⇒ steps 3-5 would be validating a different binary (this is how the
    original harness silently tested a two-month-old module)."
pass "freshly built module installed, container restarted, module loaded (sha ${FRESH_SHA:0:12}…)"

# ----------------------------------------------------------------------------
# 4. API surface — USAGE banner mentions every subcommand.
# ----------------------------------------------------------------------------
bold "[4/5] API surface"

USAGE=$(fs "uuid_audio_fork")
for sub in start stop send_text pause resume stop_play graceful-shutdown; do
    echo "$USAGE" | grep -q "$sub" || fail "USAGE banner missing '$sub' — got: $USAGE"
done
for kw in bidirectionalAudio_enabled bidirectionalAudio_stream_enabled bidirectionalAudio_stream_samplerate; do
    echo "$USAGE" | grep -q "$kw" || fail "USAGE banner missing '$kw'"
done
pass "USAGE banner includes all subcommands and bidir parameters"

# ----------------------------------------------------------------------------
# 5. Failure-mode robustness — bad inputs return errors instead of crashing.
# ----------------------------------------------------------------------------
bold "[5/5] Bad-input handling"

# Empty args → USAGE error.
OUT=$(fs "uuid_audio_fork")
echo "$OUT" | grep -q "^-USAGE:" || fail "empty args should print '-USAGE:', got: $OUT"
pass "empty args -> -USAGE"

# Non-existent UUID → graceful failure, no panic.
OUT=$(fs "uuid_audio_fork 00000000-0000-0000-0000-000000000000 start ws://localhost:1 mono 8000")
echo "$OUT" | grep -q "Operation Failed" || fail "non-existent UUID should fail gracefully, got: $OUT"
pass "non-existent UUID -> -ERR Operation Failed"

# Container must still be healthy after our pokes.
HEALTH=$(docker inspect "$FS_CONTAINER" --format '{{.State.Health.Status}}' 2>/dev/null || echo "")
[ "$HEALTH" = "healthy" ] || fail "FS container health = $HEALTH (expected healthy) — module may have crashed it"
pass "FS still healthy after probes"

echo ""
green "==> smoke tests passed"
