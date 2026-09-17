#ifndef __MEDIA_FILL_H__
#define __MEDIA_FILL_H__

#include <stddef.h>
#include <stdint.h>

/* Detect FreeSWITCH's "this frame has no real media" fill.
 *
 * ════════════════════════════════════════════════════════════════════════════
 * Why the module has to look at the bytes at all
 * ════════════════════════════════════════════════════════════════════════════
 *
 * When the channel has no real audio to decode, FreeSWITCH does not hand us
 * silence and it does not hand us an error — it hands us a frame memset to
 * 0xFF. Three paths in switch_core_io.c do it:
 *
 *     :423  CNG (comfort noise) and no PLC configured
 *     :476  PLC fill-in frame
 *     :610  the decoder returned SWITCH_STATUS_BREAK
 *
 * and switch_core_codec.c:797 fills 255 on a buffer sanity-check failure while
 * still returning SUCCESS.
 *
 * ⚠⚠ The frame carries SFF_CNG, so FreeSWITCH *knows* it is not audio — but
 *   that flag is lost when the frame is written into the media bug's
 *   raw_read_buffer. By the time switch_core_media_bug_read() hands it to us,
 *   fill and speech are the same bytes with no metadata. We forward it, and the
 *   consumer on the other end of the WebSocket cannot tell "the caller is quiet"
 *   from "there is no media on this call at all".
 *
 * ★★★ Measured, production, 2026-09-17: two calls on a real SIP trunk where the
 *   caller was talking the whole time and every single inbound sample was -1
 *   (0xFFFF, i.e. 0xFF bytes) — 307040/307040 and 310720/310720 samples. The CDR
 *   said "answered, 60s billed": indistinguishable from a caller who said
 *   nothing. It was found by scanning 96 recordings for channel energy, because
 *   nothing in the stack reported it.
 *
 * ★ This is NOT a bug to fix by dropping or zeroing the fill. The fill is
 *   correct — a media path has to produce a frame every 20ms whether or not
 *   anything arrived. What was missing is the label.
 *
 * ★★ Loopback endpoints never produce it (native L16, no RTP, no jitter buffer,
 *   no CNG/PLC, no transcoding), which is exactly why a loopback-only test suite
 *   cannot see this class of failure.
 *
 * Kept in a dependency-free header for the same reason as drop_throttle.hpp and
 * json_escape.hpp: the predicate is the part that can be silently wrong, and a
 * translation unit that pulls in switch.h cannot be unit-tested.
 */

/* mod_af_frame_is_fill returns 1 iff every byte of the frame is 0xFF.
 *
 * ★ The test is **all** bytes, not "most". FreeSWITCH memsets the whole buffer,
 *   so there is no partial state to accommodate. A threshold would only start
 *   misclassifying genuinely quiet audio — and quiet audio is precisely what
 *   this must never flag. (µ-law decodes near-silence to ±8 / ±24, never 0xFF.)
 *
 * ★★ An empty frame is not fill. "Nothing was read" and "a frame of fill was
 *   read" are different events and the caller acts differently on them.
 */
static inline int mod_af_frame_is_fill(const void *data, size_t len) {
  const uint8_t *p = (const uint8_t *) data;
  size_t i;

  if (!p || len == 0) return 0;
  for (i = 0; i < len; i++) {
    if (p[i] != 0xFF) return 0;
  }
  return 1;
}

#endif
