#ifndef __MOD_FORK_H__
#define __MOD_FORK_H__

#include <switch.h>
#include <libwebsockets.h>
#include <speex/speex_resampler.h>

#include <unistd.h>

#define MY_BUG_NAME "audio_fork"
#define MAX_BUG_LEN (64)
#define MAX_SESSION_ID (256)
#define MAX_WS_URL_LEN (512)
#define MAX_PATH_LEN (4096)

#define EVENT_TRANSCRIPTION   "mod_audio_fork::transcription"
#define EVENT_TRANSFER        "mod_audio_fork::transfer"
#define EVENT_PLAY_AUDIO      "mod_audio_fork::play_audio"
#define EVENT_KILL_AUDIO      "mod_audio_fork::kill_audio"
#define EVENT_DISCONNECT      "mod_audio_fork::disconnect"
#define EVENT_ERROR           "mod_audio_fork::error"
#define EVENT_CONNECT_SUCCESS "mod_audio_fork::connect"
#define EVENT_CONNECT_FAIL    "mod_audio_fork::connect_failed"
#define EVENT_BUFFER_OVERRUN  "mod_audio_fork::buffer_overrun"
#define EVENT_JSON            "mod_audio_fork::json"
/* PR-2: trylock contention in fork_frame. Throttled — see lws_glue.cpp. */
#define EVENT_FRAME_DROPPED   "mod_audio_fork::frame_dropped"
/* The inbound stream is FreeSWITCH's no-media fill, not audio. Throttled.
 * ⚠ Reserve AND free this in mod_audio_fork.c — see the note next to
 *   switch_event_free_subclass there. */
#define EVENT_MEDIA_SILENT    "mod_audio_fork::media_silent"

#define MAX_METADATA_LEN (8192)

/* ════════════════════════════════════════════════════════════════════════════
 * PR-3: the module reports its own identity and capabilities
 * ════════════════════════════════════════════════════════════════════════════
 *
 * ★★★ Why this is load-bearing rather than a nicety:
 *
 *   "Is FreeSWITCH running the code I just built?" had no answer. `reload
 *   mod_audio_fork` replies "+OK Reloading XML" and then "-ERR ... Module in
 *   use." — the first line matched a grep in our own smoke test while the
 *   unload had plainly failed, so a new .so sat on disk and the OLD code kept
 *   running. A file-level sha256 proves the FILE, never the RUNNING CODE.
 *   (Measured: the parser rejected port 99999 in its unit tests while FS logged
 *   "port 99999" happily. Hours went into that.)
 *
 *   ⇒ A module that states its own version closes that question for good. The
 *     harness currently restarts the container to be sure; that is a workaround
 *     for the absence of this API.
 *
 * ── Version ──
 *
 * Semver, bumped by hand. ⚠ It is NOT derived from git: the .so is what gets
 * deployed and a build from a dirty tree must not claim to be a tag.
 * ★ deploy pins a tag (CONSTRAINTS sequence discipline ③); this string is what
 *   you compare against that tag.
 *
 * ── Capability bits ──
 *
 * Go asks once at startup and enables features accordingly. The point is
 * fail-fast on a too-old module rather than silently assuming:
 *
 *   frame_drop_metrics  mod_audio_fork::frame_dropped is emitted (PR-2)
 *   media_silent        mod_audio_fork::media_silent is emitted (0.3.0)
 *   lockfree_writes     per-context lock-free write queue (PR-5) — NOT DONE
 *   multithread_safe    SERVICE_THREADS>1 is legal (PR-7)      — NOT DONE
 *
 * ★★★ media_silent is why 0.3.0 exists as a release rather than "some commits
 *   after the tag". The consumer (tanxun-aicc) had to detect no-media calls by
 *   scanning recorded PCM for all-(-1) samples — 96 recordings by hand — because
 *   the module knew and did not say. Now it says. A consumer that negotiates on
 *   this bit can drop the scan; one that cannot see the bit must keep it.
 *   ⚠ That decision is impossible if the version string does not move, which is
 *     exactly the state 0.2.0..HEAD was in: five commits, same "0.2.0".
 *
 * ★★ The last two are declared and hard-wired to 0. PR-5/6/7 were dropped from
 *   the roadmap (S2' measured no inflection point at the 10-concurrency target),
 *   but the BITS stay: they are the negotiation surface for the day someone
 *   needs to scale up, and deleting them means redesigning this API then.
 *   ⚠ Reporting 0 is the honest answer — a bit that lies is worse than absent.
 */
#define MOD_AUDIO_FORK_VERSION "0.3.0"
#define CAP_FRAME_DROP_METRICS  1
#define CAP_MEDIA_SILENT        1
#define CAP_LOCKFREE_WRITES     0
#define CAP_MULTITHREAD_SAFE    0

typedef void (*responseHandler_t)(switch_core_session_t* session, const char* eventName, char* json);

struct private_data {
	switch_mutex_t *mutex;
	char sessionId[MAX_SESSION_ID];
  char bugname[MAX_BUG_LEN+1];
  SpeexResamplerState *resampler;
  responseHandler_t responseHandler;
  void *pAudioPipe;
  int ws_state;
  char host[MAX_WS_URL_LEN];
  unsigned int port;
  char path[MAX_PATH_LEN];
  int sampling;
  int  channels;
  unsigned int id;
  /* ════════════════════════════════════════════════════════════════════════
   * ★★★ Three separate ints, deliberately NOT bitfields
   * ════════════════════════════════════════════════════════════════════════
   *
   * As `:1` bitfields these three shared one memory location, and they are
   * written from three different threads:
   *
   *   audio_paused        ESL thread   (fork_session_pauseresume)
   *   graceful_shutdown   ESL thread   (fork_session_graceful_shutdown)
   *   buffer_overrun_notified  media thread (fork_frame)
   *
   * ⚠ Assigning to a bitfield is a read-modify-write of the whole storage unit.
   *   Two unsynchronised threads touching "different" flags therefore overwrite
   *   each other — `uuid_audio_fork pause` could be undone by an overrun
   *   notification landing at the same moment, and nothing would report it.
   *
   * ★ Separate ints give each flag its own location, which removes the mutual
   *   clobbering. It does not make the accesses formally race-free — a fully
   *   correct version wants atomics — but private_t is a C struct shared with
   *   mod_audio_fork.c, so _Atomic here would have to be right across the C/C++
   *   boundary. Saying plainly what this does and does not fix beats a change
   *   that looks stronger than it is. The three bits cost 12 bytes; the struct
   *   already carries an 8KB metadata buffer.
   */
  int buffer_overrun_notified;
  int audio_paused;
  int graceful_shutdown;
  char initialMetadata[8192];

  /* Bidirectional audio: server-sent PCM played back to the caller via
   * SWITCH_ABC_TYPE_WRITE_REPLACE. playoutBuffer is a
   * boost::circular_buffer<int16_t>* — void* because this is a C header.
   * Only allocated when bidirectional_audio_enable is non-zero. */
  void *playoutBuffer;
  int bidirectional_audio_enable;

  /* Binary streaming mode: server sends raw L16 PCM directly over WebSocket
   * binary frames instead of base64-in-JSON. If the server's sample rate
   * differs from the channel rate, bidirectional_audio_resampler converts
   * the incoming PCM on the fly. set_aside_byte / has_set_aside_byte hold
   * the trailing odd byte across WS frames so we never split a sample. */
  int bidirectional_audio_stream;
  int bidirectional_audio_sample_rate;
  SpeexResamplerState *bidirectional_audio_resampler;
  uint8_t set_aside_byte;
  int has_set_aside_byte;

  /* Playback synchronization markers (mark / clearMarks protocol).
   * pendingMarks is a std::deque<PendingMark>* — see lws_glue.cpp.
   * playoutSamplesDrained is a monotonic counter incremented as
   * dub_speech_frame consumes samples; each mark records a target sample
   * index and fires once the counter passes it. */
  void *pendingMarks;
  uint64_t playoutSamplesDrained;

  /* ════════════════════════════════════════════════════════════════════════
   * PR-2: make trylock contention in fork_frame visible
   * ════════════════════════════════════════════════════════════════════════
   *
   * fork_frame() takes tech_pvt->mutex with switch_mutex_trylock and, on
   * failure, simply returns — the frame's drain is skipped. That is not lost
   * audio (the backlog is read on the next successful acquire, see A10) but it
   * IS jitter, and it was **completely invisible**: no counter, no event, no
   * log. `dc_fork_frame_dropped_total` existed on the Go side with a comment
   * saying "available after PR-2" and had no writer at all.
   *
   * ⚠ Do not add the counter and then optimise the lock (PR-5/6). Without a
   *   number first, "did it get better?" can only be answered from CPU curves.
   *   That ordering is CONSTRAINTS.md's sequence discipline ①.
   *
   * framesDroppedLock  monotonic, never reset — a rate is computed by the
   *                    consumer, and a counter that resets loses the history
   *                    exactly when someone finally looks at it.
   * lastDropReportedAt last report time in **frames** (not wall clock), so the
   *                    throttle needs no clock call on the audio path. */
  uint64_t framesDroppedLock;
  uint64_t lastDropReportedAt;

  /* ════════════════════════════════════════════════════════════════════════
   * "What we are forwarding is fill, not audio"
   * ════════════════════════════════════════════════════════════════════════
   *
   * fillFramesTotal        cumulative frames that were entirely 0xFF
   * fillFramesConsecutive  the current unbroken run; reset by any real frame
   * lastFillReportedAt     value of the run length at the previous report
   * fillReportEvery        run length between reports, in frames
   *
   * ★ The report is driven by the CONSECUTIVE run, not the total. A healthy
   *   call starts with a few fill frames while media comes up (measured: 4
   *   frames / 80ms on a working trunk call) and may show brief gaps later.
   *   What is worth an event is "N seconds with nothing at all", and only the
   *   run length says that.
   *
   * ★★ The total still rides along in the payload: after the fact you want to
   *   know how much of the call was fill, and a consumer that only sees the
   *   current run cannot reconstruct it.
   */
  uint64_t fillFramesTotal;
  uint64_t fillFramesConsecutive;
  uint64_t lastFillReportedAt;
  uint64_t fillReportEvery;
  /* How many skipped frames between reports. Default FRAME_DROP_REPORT_EVERY;
   * override per call with the channel variable
   * MOD_AUDIO_FORK_DROP_REPORT_EVERY.
   *
   * ★ Settable for two reasons, and the second is not "so tests can pass":
   *   · a test cannot otherwise provoke a report — 500 skipped frames is ~10s
   *     of *sustained contention*, which a 15-second suite will not produce
   *   · an operator chasing jitter on one call wants it turned down, and
   *     rebuilding the module to do that is not a real option */
  uint64_t dropReportEvery;
};

typedef struct private_data private_t;

#endif
