/* Unit tests for the "FreeSWITCH no-media fill" predicate.
 *
 * ★ This is the first test in the suite that touches the READ path at all.
 *   Everything else asserts that binary frames *flowed* (tests/protocol_test.sh
 *   checks `>= 5 frames`) and the mock server only logs len(message) — so a
 *   fork_frame that emitted 320 bytes of 0xFF every 20ms would pass the whole
 *   suite. This predicate is what makes that case distinguishable.
 */

#include "../media_fill.hpp"

#include <cstdio>
#include <cstring>

static int failures = 0;
static int checks = 0;

static void want(int got, int expect, const char *what) {
  checks++;
  if (got == expect) {
    std::printf("  \033[32mok\033[0m    %s\n", what);
    return;
  }
  std::printf("  \033[31mFAIL\033[0m  %s (got %d, want %d)\n", what, got, expect);
  failures++;
}

int main() {
  std::printf("media_fill\n");

  unsigned char frame[320];

  /* ── the case this exists for: a whole 20ms frame of fill ── */
  std::memset(frame, 0xFF, sizeof(frame));
  want(mod_af_frame_is_fill(frame, sizeof(frame)), 1, "320 bytes of 0xFF -> fill");

  /* ── one real sample anywhere makes it audio ──
   *
   * ★ Checked at both ends and in the middle: an early-exit loop that got the
   *   bounds wrong would still pass a middle-only test. */
  const size_t positions[] = { 0, sizeof(frame) / 2, sizeof(frame) - 1 };
  for (size_t i = 0; i < sizeof(positions) / sizeof(positions[0]); i++) {
    const size_t pos = positions[i];
    std::memset(frame, 0xFF, sizeof(frame));
    frame[pos] = 0xF8;
    char what[64];
    std::snprintf(what, sizeof(what), "one non-0xFF byte at %zu -> audio", pos);
    want(mod_af_frame_is_fill(frame, sizeof(frame)), 0, what);
  }

  /* ── quiet audio must never be flagged ──
   *
   * µ-law decodes near-silence to ±8 / ±24, never to 0xFF. Getting this wrong
   * would report "no media" on every quiet caller, which is worse than the
   * silence it is meant to detect. */
  const int16_t quiet[4] = { -8, 8, -24, 8 };
  want(mod_af_frame_is_fill(quiet, sizeof(quiet)), 0, "mu-law quiet (-8/+8/-24) -> audio");

  /* ── digital zero is not fill ──
   * All-zero means "silence was decoded", all-0xFF means "nothing was decoded".
   * Different events; conflating them loses the distinction this PR adds. */
  int16_t zeros[8];
  std::memset(zeros, 0, sizeof(zeros));
  want(mod_af_frame_is_fill(zeros, sizeof(zeros)), 0, "all-zero PCM -> audio, not fill");

  /* ── degenerate inputs ──
   * "nothing was read" and "a frame of fill was read" are different events. */
  want(mod_af_frame_is_fill(nullptr, 320), 0, "null data -> not fill");
  want(mod_af_frame_is_fill(frame, 0), 0, "zero length -> not fill");

  /* ── a single 0xFF byte still counts ──
   * The predicate must not depend on a particular frame size; ptime and codec
   * both vary. */
  const unsigned char one = 0xFF;
  want(mod_af_frame_is_fill(&one, 1), 1, "single 0xFF byte -> fill");

  std::printf(failures ? "\033[31m==> media_fill: %d/%d checks FAILED\033[0m\n"
                       : "\033[32m==> media_fill: %d checks passed\033[0m\n",
              failures ? failures : checks, checks);
  return failures ? 1 : 0;
}
