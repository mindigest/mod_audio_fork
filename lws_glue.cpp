#include <switch.h>
#include <switch_json.h>
#include <string.h>
#include <string>
#include <mutex>
#include <thread>
#include <list>
#include <algorithm>
#include <functional>
#include <cassert>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <deque>
#include <vector>

#include <boost/circular_buffer.hpp>

#include "base64.hpp"
#include "parser.hpp"
#include "mod_audio_fork.h"
#include "ws_uri.hpp"
#include "drop_throttle.hpp"
#include "media_fill.hpp"
#include "json_escape.hpp"
#include <inttypes.h>

/* ★ How many skipped frames between reports. 500 ≈ 10s at 50 frames/s.
 *   Counted in frames rather than milliseconds so the audio path needs no
 *   clock syscall — see the else-branch in fork_frame. */
#define FRAME_DROP_REPORT_EVERY 500

/* ★ How many CONSECUTIVE fill frames before saying so. 250 ≈ 5s at 50 frames/s.
 *   Short enough to catch a dead call while it is still up, long enough that a
 *   working call's start-up gap (measured: 4 frames) never trips it. */
#define MEDIA_FILL_REPORT_EVERY 250
#include "audio_pipe.hpp"

#define RTP_PACKETIZATION_PERIOD 20
#define FRAME_SIZE_8000  320 /*which means each 20ms frame as 320 bytes at 8 khz (1 channel only)*/

/* Upper bound on the in-memory playout buffer (server → caller). Capped so a
 * hostile or buggy server can't grow the buffer unbounded. 160000 samples is
 * ~10s at 16kHz; older data gets evicted in FIFO order. */
#define PLAYOUT_BUFFER_MAX_SAMPLES (160000)

/* Soft cap on pending playback markers; new marks beyond this are dropped
 * with a warning so a misbehaving server can't grow the queue unbounded. */
#define MAX_PENDING_MARKS (30)

typedef boost::circular_buffer<int16_t> PlayoutBuffer;

struct PendingMark {
  std::string name;
  uint64_t targetSample;  // fire once playoutSamplesDrained reaches this value
};
typedef std::deque<PendingMark> PendingMarkQueue;

namespace {
  static const char *requestedBufferSecs = std::getenv("MOD_AUDIO_FORK_BUFFER_SECS");
  static int nAudioBufferSecs = std::max(1, std::min(requestedBufferSecs ? ::atoi(requestedBufferSecs) : 2, 5));
  static const char *requestedNumServiceThreads = std::getenv("MOD_AUDIO_FORK_SERVICE_THREADS");
  static const char* mySubProtocolName = std::getenv("MOD_AUDIO_FORK_SUBPROTOCOL_NAME") ?
    std::getenv("MOD_AUDIO_FORK_SUBPROTOCOL_NAME") : "audio.drachtio.org";
  static unsigned int nServiceThreads = std::max(1, std::min(requestedNumServiceThreads ? ::atoi(requestedNumServiceThreads) : 1, 5));
  static unsigned int idxCallCount = 0;

  /* Build and send a {"type":"mark","data":{"name":"...","event":"..."}}
   * frame back to the server. Caller may hold tech_pvt->mutex (the AudioPipe
   * write path only acquires its own internal text mutex).
   *
   * ★★★ PR-4: the name is now **escaped**. The old comment said it was
   *   "passed through verbatim; callers are responsible ... (the server is the
   *   source of these names so this is normally already true)".
   *
   *   ⚠ The server being the source is exactly the problem: a name containing a
   *     `"` makes OUR reply malformed JSON, and the server then fails to parse
   *     a frame it caused. The failure surfaces on the far side of the wire from
   *     its cause. ★ A name ending in `","event":"...` can even close the frame
   *     early and hand the server a structurally different object.
   *     (docs/CONSTRAINTS.md A11; escaper unit-tested in tests/json_escape_test.cpp)
   *
   * ★ eventType is NOT escaped: it is one of three compile-time literals
   *   ("playout" / "cleared" / …) chosen by us, never by the peer. Escaping it
   *   would suggest it were untrusted, and that misleads the next reader. */
  void sendMarkEvent(private_t* tech_pvt, const std::string& name, const char* eventType) {
    AudioPipe* ap = static_cast<AudioPipe*>(tech_pvt->pAudioPipe);
    if (!ap) return;
    std::ostringstream json;
    json << "{\"type\":\"mark\",\"data\":{\"name\":\"" << json_escape(name)
         << "\",\"event\":\"" << eventType << "\"}}";
    ap->bufferForSending(json.str().c_str());
  }

  /* Fire and drop every queued mark with the given event type. Caller must
   * already hold tech_pvt->mutex. Used by clearMarks (eventType="cleared"),
   * killAudio, and stop_play. */
  void flushPendingMarksLocked(private_t* tech_pvt, const char* eventType) {
    if (!tech_pvt->pendingMarks) return;
    PendingMarkQueue* q = static_cast<PendingMarkQueue*>(tech_pvt->pendingMarks);
    while (!q->empty()) {
      sendMarkEvent(tech_pvt, q->front().name, eventType);
      q->pop_front();
    }
  }

  /* Drop all pending PCM in the playout buffer (and fire any pending marks
   * as cleared, since they can no longer be reached). Safe to call on a
   * tech_pvt without a playoutBuffer (no-op in that case). Used by killAudio
   * (server-initiated) and stop_play (API-initiated). */
  void clearPlayoutBuffer(private_t* tech_pvt) {
    if (!tech_pvt || !tech_pvt->playoutBuffer) return;
    switch_mutex_lock(tech_pvt->mutex);
    PlayoutBuffer* buf = static_cast<PlayoutBuffer*>(tech_pvt->playoutBuffer);
    buf->clear();
    flushPendingMarksLocked(tech_pvt, "cleared");
    switch_mutex_unlock(tech_pvt->mutex);
  }

  void processIncomingMessage(private_t* tech_pvt, switch_core_session_t* session, const char* message) {
    std::string msg = message;
    std::string type;
    cJSON* json = parse_json(session, msg, type) ;
    if (json) {
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "(%u) processIncomingMessage - received %s message\n", tech_pvt->id, type.c_str());
      cJSON* jsonData = cJSON_GetObjectItem(json, "data");
      if (0 == type.compare("playAudio")) {
        /* In-memory playout: base64-decoded raw L16 PCM is appended to a
         * bounded circular buffer; dub_speech_frame drains it into the
         * caller's outgoing RTP via SWITCH_ABC_TYPE_WRITE_REPLACE.
         *
         * Only the "raw" content type is supported here — sample-rate
         * conversion is not done in Phase C, so the server is expected to
         * send PCM at the channel's sample rate. */
        if (!tech_pvt->bidirectional_audio_enable || !tech_pvt->playoutBuffer) {
          switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
            "(%u) processIncomingMessage - playAudio received but bidirectional audio is disabled, ignoring\n",
            tech_pvt->id);
        }
        else if (tech_pvt->bidirectional_audio_stream) {
          // The server picked binary streaming mode; mixing in JSON+base64
          // playback would interleave incompatible sample rates. Drop with a
          // warning so the protocol mismatch is visible.
          switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
            "(%u) processIncomingMessage - playAudio JSON received in binary streaming mode, ignoring\n",
            tech_pvt->id);
        }
        else if (!jsonData) {
          switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
            "(%u) processIncomingMessage - missing data payload in playAudio request\n", tech_pvt->id);
        }
        else {
          const char* szAudioContentType = cJSON_GetObjectCstr(jsonData, "audioContentType");
          cJSON* jsonAudio = cJSON_GetObjectItem(jsonData, "audioContent");
          const char* szAudio = (jsonAudio && cJSON_IsString(jsonAudio)) ? jsonAudio->valuestring : nullptr;

          if (!szAudioContentType || 0 != strcmp(szAudioContentType, "raw")) {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
              "(%u) processIncomingMessage - unsupported audioContentType '%s' (only 'raw' is supported)\n",
              tech_pvt->id, szAudioContentType ? szAudioContentType : "(null)");
          }
          else if (!szAudio) {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
              "(%u) processIncomingMessage - missing audioContent in playAudio request\n", tech_pvt->id);
          }
          else {
            std::string rawAudio = drachtio::base64_decode(szAudio);
            const int16_t* samples = reinterpret_cast<const int16_t*>(rawAudio.data());
            size_t numSamples = rawAudio.size() / sizeof(int16_t);

            switch_mutex_lock(tech_pvt->mutex);
            PlayoutBuffer* buf = static_cast<PlayoutBuffer*>(tech_pvt->playoutBuffer);
            buf->insert(buf->end(), samples, samples + numSamples);
            switch_mutex_unlock(tech_pvt->mutex);

            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG,
              "(%u) processIncomingMessage - buffered %zu samples (playout size now %zu/%zu)\n",
              tech_pvt->id, numSamples, buf->size(), buf->capacity());
          }
          tech_pvt->responseHandler(session, EVENT_PLAY_AUDIO, NULL);
        }
      }
      else if (0 == type.compare("killAudio")) {
        // Stop both kinds of playback the caller might be hearing:
        //   * in-memory playoutBuffer drained by dub_speech_frame (our own
        //     bidirectional path)
        //   * any active playback() from the dialplan (file-based)
        clearPlayoutBuffer(tech_pvt);
        switch_channel_t *channel = switch_core_session_get_channel(session);
        switch_channel_set_flag_value(channel, CF_BREAK, 2);
        tech_pvt->responseHandler(session, EVENT_KILL_AUDIO, NULL);
      }
      else if (0 == type.compare("mark")) {
        // Register a playback marker — fires a "playout" event back to the
        // server when all currently-buffered audio has been played.
        if (!tech_pvt->bidirectional_audio_enable || !tech_pvt->pendingMarks) {
          switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
            "(%u) processIncomingMessage - mark received but bidirectional audio is disabled, ignoring\n",
            tech_pvt->id);
        }
        else if (!jsonData) {
          switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
            "(%u) processIncomingMessage - missing data payload in mark request\n", tech_pvt->id);
        }
        else {
          cJSON* jsonName = cJSON_GetObjectItem(jsonData, "name");
          if (!jsonName || !cJSON_IsString(jsonName) || !jsonName->valuestring) {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
              "(%u) processIncomingMessage - mark missing 'name'\n", tech_pvt->id);
          }
          else {
            switch_mutex_lock(tech_pvt->mutex);
            PendingMarkQueue* q = static_cast<PendingMarkQueue*>(tech_pvt->pendingMarks);
            if (q->size() >= MAX_PENDING_MARKS) {
              switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
                "(%u) processIncomingMessage - pending mark queue full (%d), dropping '%s'\n",
                tech_pvt->id, MAX_PENDING_MARKS, jsonName->valuestring);
            }
            else {
              PlayoutBuffer* buf = static_cast<PlayoutBuffer*>(tech_pvt->playoutBuffer);
              uint64_t targetSample = tech_pvt->playoutSamplesDrained + (buf ? buf->size() : 0);
              q->push_back({ std::string(jsonName->valuestring), targetSample });
              switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG,
                "(%u) processIncomingMessage - queued mark '%s' at sample %llu\n",
                tech_pvt->id, jsonName->valuestring, (unsigned long long)targetSample);
            }
            switch_mutex_unlock(tech_pvt->mutex);
          }
        }
      }
      else if (0 == type.compare("clearMarks")) {
        // Drop all pending markers and notify the server that each was cleared.
        // Audio playout is NOT interrupted (use killAudio / stop_play for that).
        if (tech_pvt->pendingMarks) {
          switch_mutex_lock(tech_pvt->mutex);
          flushPendingMarksLocked(tech_pvt, "cleared");
          switch_mutex_unlock(tech_pvt->mutex);
        }
      }
      else if (0 == type.compare("transcription")) {
        char* jsonString = cJSON_PrintUnformatted(jsonData);
        tech_pvt->responseHandler(session, EVENT_TRANSCRIPTION, jsonString);
        free(jsonString);        
      }
      else if (0 == type.compare("transfer")) {
        char* jsonString = cJSON_PrintUnformatted(jsonData);
        tech_pvt->responseHandler(session, EVENT_TRANSFER, jsonString);
        free(jsonString);                
      }
      else if (0 == type.compare("disconnect")) {
        char* jsonString = cJSON_PrintUnformatted(jsonData);
        tech_pvt->responseHandler(session, EVENT_DISCONNECT, jsonString);
        free(jsonString);        
      }
      else if (0 == type.compare("error")) {
        char* jsonString = cJSON_PrintUnformatted(jsonData);
        tech_pvt->responseHandler(session, EVENT_ERROR, jsonString);
        free(jsonString);        
      }
      else if (0 == type.compare("json")) {
        char* jsonString = cJSON_PrintUnformatted(json);
        tech_pvt->responseHandler(session, EVENT_JSON, jsonString);
        free(jsonString);
      }
      else {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "(%u) processIncomingMessage - unsupported msg type %s\n", tech_pvt->id, type.c_str());  
      }
      cJSON_Delete(json);
    }
    else {
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "(%u) processIncomingMessage - could not parse message: %s\n", tech_pvt->id, message);
    }
  }

  /* Handle a chunk of raw L16 PCM that arrived over a WebSocket binary frame.
   * Steps:
   *   1. Restitch any odd byte left over from the previous frame so we never
   *      split an int16_t sample across frame boundaries.
   *   2. Resample if the server's sample rate differs from the channel's.
   *   3. Append to the playoutBuffer for dub_speech_frame to drain.
   * Designed for low latency — each chunk is processed immediately, no
   * separate prebuffer (the playoutBuffer itself absorbs jitter, and
   * dub_speech_frame fills silence on underrun). */
  void processIncomingBinary(private_t* tech_pvt, const char* data, size_t dataLength) {
    if (!tech_pvt->bidirectional_audio_enable || !tech_pvt->playoutBuffer) return;
    if (dataLength == 0) return;

    // (1) Reassemble across odd-byte boundaries.
    std::vector<uint8_t> raw;
    raw.reserve(dataLength + 1);
    if (tech_pvt->has_set_aside_byte) {
      raw.push_back(tech_pvt->set_aside_byte);
      tech_pvt->has_set_aside_byte = 0;
    }
    raw.insert(raw.end(), reinterpret_cast<const uint8_t*>(data),
                          reinterpret_cast<const uint8_t*>(data) + dataLength);
    if (raw.size() % 2 != 0) {
      tech_pvt->set_aside_byte = raw.back();
      tech_pvt->has_set_aside_byte = 1;
      raw.pop_back();
    }
    if (raw.empty()) return;

    const int16_t* inSamples = reinterpret_cast<const int16_t*>(raw.data());
    const size_t inSampleCount = raw.size() / sizeof(int16_t);

    // (2) Resample if needed. Otherwise just adopt the buffer directly.
    std::vector<int16_t> out;
    if (tech_pvt->bidirectional_audio_resampler) {
      // Speex max upsampling factor we'd realistically see is 8k → 48k = 6x.
      out.resize(inSampleCount * 6);
      spx_uint32_t in_len = static_cast<spx_uint32_t>(inSampleCount);
      spx_uint32_t out_len = static_cast<spx_uint32_t>(out.size());
      int err = speex_resampler_process_interleaved_int(
          tech_pvt->bidirectional_audio_resampler,
          inSamples, &in_len, out.data(), &out_len);
      if (err != 0) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
          "(%u) processIncomingBinary - resampler error: %s\n",
          tech_pvt->id, speex_resampler_strerror(err));
        return;
      }
      out.resize(out_len);
    }
    else {
      out.assign(inSamples, inSamples + inSampleCount);
    }
    if (out.empty()) return;

    // (3) Append under the session mutex.
    switch_mutex_lock(tech_pvt->mutex);
    PlayoutBuffer* buf = static_cast<PlayoutBuffer*>(tech_pvt->playoutBuffer);
    buf->insert(buf->end(), out.begin(), out.end());
    switch_mutex_unlock(tech_pvt->mutex);
  }

  /* noteFillFrame records one inbound frame and reports a sustained run of
   * FreeSWITCH no-media fill.
   *
   * ★★★ The frame is still forwarded either way. The fill is not wrong — a
   *   media path must produce a frame every 20ms whether or not anything
   *   arrived — and dropping it here would turn a diagnosable problem into a
   *   stream with holes in it. What was missing is the label, so that is all
   *   this adds. See media_fill.hpp for why the label cannot come from
   *   FreeSWITCH itself (SFF_CNG is lost on the way into the bug buffer).
   *
   * ★ Called on the audio path, so: no clock syscall, no allocation, and the
   *   throttle counts frames. Same constraints as the PR-2 drop counter, and it
   *   reuses that counter's arithmetic rather than open-coding a second copy.
   */
  void noteFillFrame(switch_core_session_t* session, private_t* tech_pvt, int isFill) {
    if (!tech_pvt) return;
    if (!isFill) {
      /* ★ Reset the report marker too, not just the run. Otherwise a second
       *   outage in the same call stays silent until it exceeds the first. */
      tech_pvt->fillFramesConsecutive = 0;
      tech_pvt->lastFillReportedAt = 0;
      return;
    }
    tech_pvt->fillFramesTotal++;
    tech_pvt->fillFramesConsecutive++;
    if (!mod_af_should_report_drop(tech_pvt->fillFramesConsecutive,
                                   tech_pvt->lastFillReportedAt,
                                   tech_pvt->fillReportEvery,
                                   MEDIA_FILL_REPORT_EVERY)) {
      return;
    }
    tech_pvt->lastFillReportedAt = tech_pvt->fillFramesConsecutive;
    char buf[192];
    snprintf(buf, sizeof(buf),
      "{\"consecutive_fill_frames\":%" PRIu64 ",\"consecutive_ms\":%" PRIu64
      ",\"total_fill_frames\":%" PRIu64 "}",
      tech_pvt->fillFramesConsecutive,
      tech_pvt->fillFramesConsecutive * RTP_PACKETIZATION_PERIOD,
      tech_pvt->fillFramesTotal);
    tech_pvt->responseHandler(session, EVENT_MEDIA_SILENT, buf);
    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING,
      "(%u) mod_audio_fork: inbound is FreeSWITCH no-media fill, not audio "
      "(%" PRIu64 " consecutive frames = %" PRIu64 "ms)\n",
      tech_pvt->id, tech_pvt->fillFramesConsecutive,
      tech_pvt->fillFramesConsecutive * RTP_PACKETIZATION_PERIOD);
  }

  /* ════════════════════════════════════════════════════════════════════════
   * tech_pvt->pAudioPipe must only be touched under tech_pvt->mutex
   * ════════════════════════════════════════════════════════════════════════
   *
   * The pointer is written on the **lws service thread** (the three cases in
   * eventCallback below) and read + dereferenced on the **FreeSWITCH media
   * thread** (fork_frame). Worse, the object itself is deleted on the lws
   * thread immediately after the callback returns — audio_pipe.cpp,
   * LWS_CALLBACK_CLIENT_CLOSED:
   *
   *     *ppAp = NULL;
   *     delete ap;
   *
   * ⚠ So the media thread could be holding a pointer that the lws thread is
   *   about to free: it loads tech_pvt->pAudioPipe, and between that load and
   *   pAudioPipe->lockAudioBuffer() the object goes away. Use-after-free.
   *
   * ★ Nulling the pointer under the same mutex fork_frame uses closes it:
   *   a media thread that already holds the mutex finishes first, and one that
   *   acquires it afterwards sees nullptr. Since `delete ap` happens *after*
   *   m_callback() returns, by then nobody can be inside.
   *
   * ★★ Lock order is safe. Everything that takes tech_pvt->mutex may then take
   *   an AudioPipe-internal lock (m_text_mutex / m_audio_mutex), never the
   *   reverse: the lws thread releases mutex_connects/writes/disconnects before
   *   invoking any callback, and AudioPipe::close() / bufferForSending() are
   *   non-blocking (they only queue and lws_cancel_service). Verified by
   *   reading every lock site, not by assumption — a blocking close() here
   *   would deadlock against an lws thread waiting on tech_pvt->mutex.
   */
  void clearAudioPipe(private_t* tech_pvt) {
    if (!tech_pvt || !tech_pvt->mutex) {
      if (tech_pvt) tech_pvt->pAudioPipe = nullptr;
      return;
    }
    switch_mutex_lock(tech_pvt->mutex);
    tech_pvt->pAudioPipe = nullptr;
    switch_mutex_unlock(tech_pvt->mutex);
  }

  static void eventCallback(const char* sessionId, const char* bugname, AudioPipe::NotifyEvent_t event,
                            const char* message, const char* binary, size_t binary_len) {
    switch_core_session_t* session = switch_core_session_locate(sessionId);
    if (session) {
      switch_channel_t *channel = switch_core_session_get_channel(session);
      switch_media_bug_t *bug = (switch_media_bug_t*) switch_channel_get_private(channel, bugname);
      if (bug) {
        private_t* tech_pvt = (private_t*) switch_core_media_bug_get_user_data(bug);
        if (tech_pvt) {
          switch (event) {
            case AudioPipe::CONNECT_SUCCESS:
              switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "connection successful\n");
              tech_pvt->responseHandler(session, EVENT_CONNECT_SUCCESS, NULL);
              if (strlen(tech_pvt->initialMetadata) > 0) {
                switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "sending initial metadata %s\n", tech_pvt->initialMetadata);
                /* ★ Under the mutex, and null-checked. This was an unchecked
                 *   dereference: on the CONNECT_SUCCESS → (racing) teardown
                 *   ordering, pAudioPipe can already be nullptr here. */
                switch_mutex_lock(tech_pvt->mutex);
                AudioPipe *pAudioPipe = static_cast<AudioPipe *>(tech_pvt->pAudioPipe);
                if (pAudioPipe) pAudioPipe->bufferForSending(tech_pvt->initialMetadata);
                switch_mutex_unlock(tech_pvt->mutex);
              }
            break;
            case AudioPipe::CONNECT_FAIL:
            {
              // first thing: we can no longer access the AudioPipe
              std::stringstream json;
              json << "{\"reason\":\"" << message << "\"}";
              clearAudioPipe(tech_pvt);
              tech_pvt->responseHandler(session, EVENT_CONNECT_FAIL, (char *) json.str().c_str());
              switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_NOTICE, "connection failed: %s\n", message);
            }
            break;
            case AudioPipe::CONNECTION_DROPPED:
              // first thing: we can no longer access the AudioPipe
              clearAudioPipe(tech_pvt);
              tech_pvt->responseHandler(session, EVENT_DISCONNECT, NULL);
              switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_NOTICE, "connection dropped from far end\n");
            break;
            case AudioPipe::CONNECTION_CLOSED_GRACEFULLY:
              // first thing: we can no longer access the AudioPipe
              clearAudioPipe(tech_pvt);
              switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "connection closed gracefully\n");
            break;
            case AudioPipe::MESSAGE:
              processIncomingMessage(tech_pvt, session, message);
            break;
            case AudioPipe::BINARY:
              processIncomingBinary(tech_pvt, binary, binary_len);
            break;
          }
        }
      }
      switch_core_session_rwunlock(session);
    }
  }
  switch_status_t fork_data_init(private_t *tech_pvt, switch_core_session_t *session, char * host,
    unsigned int port, char* path, int sslFlags, int sampling, int desiredSampling, int channels,
    char *bugname, char* metadata,
    int bidirectional_audio_enable, int bidirectional_audio_stream, int bidirectional_audio_sample_rate,
    responseHandler_t responseHandler) {

    const char* username = nullptr;
    const char* password = nullptr;
    int err;
    switch_codec_implementation_t read_impl;
    switch_channel_t *channel = switch_core_session_get_channel(session);

    switch_core_session_get_read_impl(session, &read_impl);

    if ((username = switch_channel_get_variable(channel, "MOD_AUDIO_BASIC_AUTH_USERNAME"))) {
      password = switch_channel_get_variable(channel, "MOD_AUDIO_BASIC_AUTH_PASSWORD");
    }

    memset(tech_pvt, 0, sizeof(private_t));

    strncpy(tech_pvt->sessionId, switch_core_session_get_uuid(session), MAX_SESSION_ID - 1);
    strncpy(tech_pvt->host, host, MAX_WS_URL_LEN - 1);
    tech_pvt->port = port;
    strncpy(tech_pvt->path, path, MAX_PATH_LEN - 1);
    tech_pvt->sampling = desiredSampling;
    tech_pvt->responseHandler = responseHandler;
    tech_pvt->channels = channels;
    tech_pvt->id = ++idxCallCount;
    tech_pvt->buffer_overrun_notified = 0;
    /* PR-2 report throttle. ★ Guard against 0: it would fire an event on every
     *   single skipped frame, and at 50 fps × N sessions that turns the
     *   measurement into the bottleneck it is measuring. */
    tech_pvt->dropReportEvery = FRAME_DROP_REPORT_EVERY;
    {
      const char *v = switch_channel_get_variable(
        switch_core_session_get_channel(session), "MOD_AUDIO_FORK_DROP_REPORT_EVERY");
      if (v) {
        long n = atol(v);
        if (n > 0) tech_pvt->dropReportEvery = (uint64_t) n;
        else switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING,
          "MOD_AUDIO_FORK_DROP_REPORT_EVERY=%s ignored (must be > 0)\n", v);
      }
    }
    tech_pvt->fillReportEvery = MEDIA_FILL_REPORT_EVERY;
    {
      const char *v = switch_channel_get_variable(
        switch_core_session_get_channel(session), "MOD_AUDIO_FORK_FILL_REPORT_EVERY");
      if (v) {
        long n = atol(v);
        if (n > 0) tech_pvt->fillReportEvery = (uint64_t) n;
        else switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING,
          "MOD_AUDIO_FORK_FILL_REPORT_EVERY=%s ignored (must be > 0)\n", v);
      }
    }
    tech_pvt->audio_paused = 0;
    tech_pvt->graceful_shutdown = 0;
    tech_pvt->bidirectional_audio_enable = bidirectional_audio_enable;
    tech_pvt->bidirectional_audio_stream = bidirectional_audio_stream;
    tech_pvt->bidirectional_audio_sample_rate = bidirectional_audio_sample_rate;
    tech_pvt->bidirectional_audio_resampler = nullptr;
    tech_pvt->has_set_aside_byte = 0;
    tech_pvt->set_aside_byte = 0;
    tech_pvt->playoutBuffer = nullptr;
    tech_pvt->pendingMarks = nullptr;
    tech_pvt->playoutSamplesDrained = 0;
    strncpy(tech_pvt->bugname, bugname, MAX_BUG_LEN);
    if (metadata) strncpy(tech_pvt->initialMetadata, metadata, MAX_METADATA_LEN - 1);

    if (bidirectional_audio_enable) {
      tech_pvt->playoutBuffer = new PlayoutBuffer(PLAYOUT_BUFFER_MAX_SAMPLES);
      tech_pvt->pendingMarks = new PendingMarkQueue();
    }

    // Set up the inbound resampler only if (a) binary streaming mode is on,
    // (b) the server's sample rate is known, and (c) it differs from the
    // channel's sample rate. JSON+base64 playAudio in Phase C assumes server
    // rate matches channel rate — no resampling there for now.
    if (bidirectional_audio_enable && bidirectional_audio_stream
        && bidirectional_audio_sample_rate > 0
        && bidirectional_audio_sample_rate != sampling) {
      int rerr = 0;
      tech_pvt->bidirectional_audio_resampler = speex_resampler_init(
          1, bidirectional_audio_sample_rate, sampling, SWITCH_RESAMPLE_QUALITY, &rerr);
      if (rerr != 0) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
          "Error initializing bidirectional audio resampler (%d → %d): %s.\n",
          bidirectional_audio_sample_rate, sampling, speex_resampler_strerror(rerr));
        return SWITCH_STATUS_FALSE;
      }
      switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG,
        "(%u) bidirectional audio resampling: server %dHz → channel %dHz\n",
        tech_pvt->id, bidirectional_audio_sample_rate, sampling);
    }
    
    size_t buflen = LWS_PRE + (FRAME_SIZE_8000 * desiredSampling / 8000 * channels * 1000 / RTP_PACKETIZATION_PERIOD * nAudioBufferSecs);

    AudioPipe* ap = new AudioPipe(tech_pvt->sessionId, host, port, path, sslFlags,
      buflen, read_impl.decoded_bytes_per_packet, username, password, bugname,
      bidirectional_audio_stream != 0, eventCallback);
    if (!ap) {
      switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Error allocating AudioPipe\n");
      return SWITCH_STATUS_FALSE;
    }

    tech_pvt->pAudioPipe = static_cast<void *>(ap);

    switch_mutex_init(&tech_pvt->mutex, SWITCH_MUTEX_NESTED, switch_core_session_get_pool(session));

    if (desiredSampling != sampling) {
      switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "(%u) resampling from %u to %u\n", tech_pvt->id, sampling, desiredSampling);
      tech_pvt->resampler = speex_resampler_init(channels, sampling, desiredSampling, SWITCH_RESAMPLE_QUALITY, &err);
      if (0 != err) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Error initializing resampler: %s.\n", speex_resampler_strerror(err));
        return SWITCH_STATUS_FALSE;
      }
    }
    else {
      switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "(%u) no resampling needed for this call\n", tech_pvt->id);
    }

    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "(%u) fork_data_init\n", tech_pvt->id);

    return SWITCH_STATUS_SUCCESS;
  }

  void destroy_tech_pvt(private_t* tech_pvt) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "%s (%u) destroy_tech_pvt\n", tech_pvt->sessionId, tech_pvt->id);
    if (tech_pvt->resampler) {
      speex_resampler_destroy(tech_pvt->resampler);
      tech_pvt->resampler = nullptr;
    }
    if (tech_pvt->bidirectional_audio_resampler) {
      speex_resampler_destroy(tech_pvt->bidirectional_audio_resampler);
      tech_pvt->bidirectional_audio_resampler = nullptr;
    }
    if (tech_pvt->playoutBuffer) {
      delete static_cast<PlayoutBuffer*>(tech_pvt->playoutBuffer);
      tech_pvt->playoutBuffer = nullptr;
    }
    if (tech_pvt->pendingMarks) {
      delete static_cast<PendingMarkQueue*>(tech_pvt->pendingMarks);
      tech_pvt->pendingMarks = nullptr;
    }
    /* ════════════════════════════════════════════════════════════════════
     * ★★★ The mutex is deliberately NOT destroyed here.
     * ════════════════════════════════════════════════════════════════════
     *
     * It is allocated from the session pool (fork_data_init calls
     * switch_mutex_init with switch_core_session_get_pool(session)), so APR
     * reclaims it when the session pool is destroyed. Destroying it here
     * bought nothing and cost two things:
     *
     *  · fork_session_cleanup calls us while HOLDING this very mutex.
     *    pthread_mutex_destroy on a locked mutex is undefined; on Linux it
     *    returns EBUSY and silently does nothing, so the bug was invisible —
     *    it looked like it worked.
     *
     *  · the lws service thread can be inside processIncomingBinary /
     *    processIncomingMessage, blocked on switch_mutex_lock(tech_pvt->mutex),
     *    at the exact moment we destroy it. Nothing in the current design
     *    excludes that. ⚠ Letting the pool own the lifetime removes the whole
     *    class of "destroyed while someone is about to lock it".
     *
     * ★ tech_pvt itself is also session-pool allocated (fork_session_init uses
     *   switch_core_session_alloc), so this is consistent: everything whose
     *   address the other thread may still hold outlives us.
     */
  }

  void lws_logger(int level, const char *line) {
    switch_log_level_t llevel = SWITCH_LOG_DEBUG;

    switch (level) {
      case LLL_ERR: llevel = SWITCH_LOG_ERROR; break;
      case LLL_WARN: llevel = SWITCH_LOG_WARNING; break;
      case LLL_NOTICE: llevel = SWITCH_LOG_NOTICE; break;
      case LLL_INFO: llevel = SWITCH_LOG_INFO; break;
      break;
    }
	  switch_log_printf(SWITCH_CHANNEL_LOG, llevel, "%s\n", line);
  }
}

extern "C" {
  int parse_ws_uri(switch_channel_t *channel, const char* szServerUri, char* host, char *path, unsigned int* pPort, int* pSslFlags) {
    size_t offset = 0;
    int secure = 0;
    int flags = LCCSCF_USE_SSL;
    const char *err = 0;

    if (switch_true(switch_channel_get_variable(channel, "MOD_AUDIO_FORK_ALLOW_SELFSIGNED"))) {
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "parse_ws_uri - allowing self-signed certs\n");
      flags |= LCCSCF_ALLOW_SELFSIGNED;
    }
    if (switch_true(switch_channel_get_variable(channel, "MOD_AUDIO_FORK_SKIP_SERVER_CERT_HOSTNAME_CHECK"))) {
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "parse_ws_uri - skipping hostname check\n");
      flags |= LCCSCF_SKIP_SERVER_CERT_HOSTNAME_CHECK;
    }
    if (switch_true(switch_channel_get_variable(channel, "MOD_AUDIO_FORK_ALLOW_EXPIRED"))) {
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "parse_ws_uri - allowing expired certs\n");
      flags |= LCCSCF_ALLOW_EXPIRED;
    }

    if (!szServerUri || !ws_uri_scheme(szServerUri, &offset, &secure, pPort)) {
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
        "parse_ws_uri - error parsing uri %s: invalid scheme (want ws/wss/http/https)\n",
        szServerUri ? szServerUri : "(null)");
      /* ★ Zero the caller's buffers on every failure path — see ws_uri.hpp. */
      host[0] = '\0';
      path[0] = '\0';
      return 0;
    }
    *pSslFlags = secure ? flags : 0;

    /* ★★★ The parsing itself lives in ws_uri.cpp, which has no FreeSWITCH or
     *     libwebsockets dependency. That split is what makes tests/ws_uri_test.cpp
     *     possible (44 table-driven cases) — the std::regex it replaced could
     *     only ever be exercised by starting a whole FreeSWITCH.
     *
     * ⚠ The buffer sizes are the CALLER's, and they are what the fixed-size
     *   stack arrays in mod_audio_fork.c declare. Passing anything larger here
     *   re-opens the overflow the old strncpy had. */
    if (!ws_uri_parse_authority(szServerUri + offset, host, MAX_WS_URL_LEN,
                                path, MAX_PATH_LEN, pPort, &err)) {
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
        "parse_ws_uri - invalid uri %s: %s\n", szServerUri, err ? err : "unparseable");
      return 0;
    }
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG,
      "parse_ws_uri - host %s, port %u, path %s\n", host, *pPort, path);

    return 1;
  }

  int fork_effective_threads(void) {
    return (int) nServiceThreads;
  }
  const char* fork_effective_subprotocol(void) {
    return mySubProtocolName;
  }

  switch_status_t fork_init() {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_audio_fork: audio buffer (in secs):    %d secs\n", nAudioBufferSecs);
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_audio_fork: sub-protocol:              %s\n", mySubProtocolName);
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_audio_fork: lws service threads:       %d\n", nServiceThreads);
 
    int logs = LLL_ERR | LLL_WARN | LLL_NOTICE ;
     //LLL_INFO | LLL_PARSER | LLL_HEADER | LLL_EXT | LLL_CLIENT  | LLL_LATENCY | LLL_DEBUG ;
    AudioPipe::initialize(mySubProtocolName, nServiceThreads, logs, lws_logger);
   return SWITCH_STATUS_SUCCESS;
  }

  switch_status_t fork_cleanup() {
    bool cleanup = false;
    cleanup = AudioPipe::deinitialize();
    if (cleanup == true) {
        return SWITCH_STATUS_SUCCESS;
    }
    return SWITCH_STATUS_FALSE;
  }

  switch_status_t fork_session_init(switch_core_session_t *session,
              responseHandler_t responseHandler,
              uint32_t samples_per_second,
              char *host,
              unsigned int port,
              char *path,
              int sampling,
              int sslFlags,
              int channels,
              char *bugname,
              char* metadata,
              int bidirectional_audio_enable,
              int bidirectional_audio_stream,
              int bidirectional_audio_sample_rate,
              void **ppUserData)
  {
    // allocate per-session data structure
    private_t* tech_pvt = (private_t *) switch_core_session_alloc(session, sizeof(private_t));
    if (!tech_pvt) {
      switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "error allocating memory!\n");
      return SWITCH_STATUS_FALSE;
    }
    if (SWITCH_STATUS_SUCCESS != fork_data_init(tech_pvt, session, host, port, path, sslFlags, samples_per_second, sampling, channels,
      bugname, metadata, bidirectional_audio_enable, bidirectional_audio_stream, bidirectional_audio_sample_rate,
      responseHandler)) {
      destroy_tech_pvt(tech_pvt);
      return SWITCH_STATUS_FALSE;
    }

    *ppUserData = tech_pvt;
    return SWITCH_STATUS_SUCCESS;
  }

   switch_status_t fork_session_connect(void **ppUserData) {
    private_t *tech_pvt = static_cast<private_t *>(*ppUserData);
    if (!tech_pvt) return SWITCH_STATUS_FALSE;
    /* ★ Was an unchecked dereference: `static_cast<AudioPipe*>(...)->connect()`
     *   with no null test at all. It happens to be non-null today because the
     *   caller runs right after fork_session_init — but "happens to be" is not
     *   a guarantee, and this is the one place a failed init would land. */
    switch_mutex_lock(tech_pvt->mutex);
    AudioPipe *pAudioPipe = static_cast<AudioPipe*>(tech_pvt->pAudioPipe);
    if (pAudioPipe) pAudioPipe->connect();
    switch_mutex_unlock(tech_pvt->mutex);
    return pAudioPipe ? SWITCH_STATUS_SUCCESS : SWITCH_STATUS_FALSE;
  }

  switch_status_t fork_session_cleanup(switch_core_session_t *session, char *bugname, char* text, int channelIsClosing) {
    switch_channel_t *channel = switch_core_session_get_channel(session);
    switch_media_bug_t *bug = (switch_media_bug_t*) switch_channel_get_private(channel, bugname);
    if (!bug) {
      switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "fork_session_cleanup: no bug %s - websocket conection already closed\n", bugname);
      return SWITCH_STATUS_FALSE;
    }
    private_t* tech_pvt = (private_t*) switch_core_media_bug_get_user_data(bug);
    /* ★ The null check has to come BEFORE the first dereference. It used to sit
     *   three lines further down, after `uint32_t id = tech_pvt->id;` — so on the
     *   path it was meant to guard we had already crashed. */
    if (!tech_pvt) return SWITCH_STATUS_FALSE;
    uint32_t id = tech_pvt->id;

    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "(%u) fork_session_cleanup\n", id);

    switch_mutex_lock(tech_pvt->mutex);
    /* ★ Read INSIDE the lock. This used to sit above the lock, so cleanup could
     *   load a pointer that the lws thread deleted a moment later — the same
     *   window fork_frame has, just on a different thread. */
    AudioPipe *pAudioPipe = static_cast<AudioPipe *>(tech_pvt->pAudioPipe);

    // get the bug again, now that we are under lock
    {
      switch_media_bug_t *bug = (switch_media_bug_t*) switch_channel_get_private(channel, bugname);
      if (bug) {
        switch_channel_set_private(channel, bugname, NULL);
        if (!channelIsClosing) {
          switch_core_media_bug_remove(session, &bug);
        }
      }
    }

    if (pAudioPipe && text) pAudioPipe->bufferForSending(text);
    if (pAudioPipe) pAudioPipe->close();

    /* ★★★ Unlock BEFORE tearing anything down.
     *
     * This function used to lock above and then return without ever unlocking,
     * while destroy_tech_pvt() destroyed the very mutex we were holding. Both
     * halves were silent: the missing unlock is invisible once the mutex is
     * gone, and pthread_mutex_destroy on a held mutex just returns EBUSY.
     *
     * ★ Everything the lock protects is done by this point: the bug is detached
     *   (switch_channel_set_private(.., NULL) above), so no NEW eventCallback
     *   can reach tech_pvt, and the pipe is closed. */
    switch_mutex_unlock(tech_pvt->mutex);

    destroy_tech_pvt(tech_pvt);
    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "(%u) fork_session_cleanup: connection closed\n", id);
    return SWITCH_STATUS_SUCCESS;
  }

  switch_status_t fork_session_send_text(switch_core_session_t *session, char *bugname, char* text) {
    switch_channel_t *channel = switch_core_session_get_channel(session);
    switch_media_bug_t *bug = (switch_media_bug_t*) switch_channel_get_private(channel, bugname);
    if (!bug) {
      switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "fork_session_send_text failed because no bug\n");
      return SWITCH_STATUS_FALSE;
    }
    private_t* tech_pvt = (private_t*) switch_core_media_bug_get_user_data(bug);
  
    if (!tech_pvt) return SWITCH_STATUS_FALSE;
    /* ★ Use it under the lock, do not just read it under the lock: the object
     *   can be deleted on the lws thread the instant we let go. */
    switch_mutex_lock(tech_pvt->mutex);
    AudioPipe *pAudioPipe = static_cast<AudioPipe *>(tech_pvt->pAudioPipe);
    if (pAudioPipe && text) pAudioPipe->bufferForSending(text);
    switch_mutex_unlock(tech_pvt->mutex);

    return SWITCH_STATUS_SUCCESS;
  }

  switch_status_t fork_session_stop_play(switch_core_session_t *session, char *bugname) {
    switch_channel_t *channel = switch_core_session_get_channel(session);
    switch_media_bug_t *bug = (switch_media_bug_t*) switch_channel_get_private(channel, bugname);
    if (!bug) {
      switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "fork_session_stop_play failed because no bug\n");
      return SWITCH_STATUS_FALSE;
    }
    private_t* tech_pvt = (private_t*) switch_core_media_bug_get_user_data(bug);
    if (!tech_pvt) return SWITCH_STATUS_FALSE;

    clearPlayoutBuffer(tech_pvt);
    return SWITCH_STATUS_SUCCESS;
  }

  switch_status_t fork_session_pauseresume(switch_core_session_t *session, char *bugname, int pause) {
    switch_channel_t *channel = switch_core_session_get_channel(session);
    switch_media_bug_t *bug = (switch_media_bug_t*) switch_channel_get_private(channel, bugname);
    if (!bug) {
      switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "fork_session_pauseresume failed because no bug\n");
      return SWITCH_STATUS_FALSE;
    }
    private_t* tech_pvt = (private_t*) switch_core_media_bug_get_user_data(bug);
  
    if (!tech_pvt) return SWITCH_STATUS_FALSE;

    switch_core_media_bug_flush(bug);
    tech_pvt->audio_paused = pause;
    return SWITCH_STATUS_SUCCESS;
  }

  switch_status_t fork_session_graceful_shutdown(switch_core_session_t *session, char *bugname) {
    switch_channel_t *channel = switch_core_session_get_channel(session);
    switch_media_bug_t *bug = (switch_media_bug_t*) switch_channel_get_private(channel, bugname);
    if (!bug) {
      switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "fork_session_graceful_shutdown failed because no bug\n");
      return SWITCH_STATUS_FALSE;
    }
    private_t* tech_pvt = (private_t*) switch_core_media_bug_get_user_data(bug);
  
    if (!tech_pvt) return SWITCH_STATUS_FALSE;

    tech_pvt->graceful_shutdown = 1;

    switch_mutex_lock(tech_pvt->mutex);
    AudioPipe *pAudioPipe = static_cast<AudioPipe *>(tech_pvt->pAudioPipe);
    if (pAudioPipe) pAudioPipe->do_graceful_shutdown();
    switch_mutex_unlock(tech_pvt->mutex);

    return SWITCH_STATUS_SUCCESS;
  }

  switch_bool_t fork_frame(switch_core_session_t *session, switch_media_bug_t *bug) {
    private_t* tech_pvt = (private_t*) switch_core_media_bug_get_user_data(bug);

    if (!tech_pvt || tech_pvt->audio_paused || tech_pvt->graceful_shutdown) return SWITCH_TRUE;
    
    if (switch_mutex_trylock(tech_pvt->mutex) == SWITCH_STATUS_SUCCESS) {
      if (!tech_pvt->pAudioPipe) {
        switch_mutex_unlock(tech_pvt->mutex);
        return SWITCH_TRUE;
      }
      AudioPipe *pAudioPipe = static_cast<AudioPipe *>(tech_pvt->pAudioPipe);
      if (pAudioPipe->getLwsState() != AudioPipe::LWS_CLIENT_CONNECTED) {
        switch_mutex_unlock(tech_pvt->mutex);
        return SWITCH_TRUE;
      }

      pAudioPipe->lockAudioBuffer();
      size_t available = pAudioPipe->binarySpaceAvailable();
      if (NULL == tech_pvt->resampler) {
        switch_frame_t frame;
        memset(&frame, 0, sizeof(frame));
        frame.data = pAudioPipe->binaryWritePtr();
        frame.buflen = available;
        while (true) {

          // check if buffer would be overwritten; dump packets if so
          if (available < pAudioPipe->binaryMinSpace()) {
            if (!tech_pvt->buffer_overrun_notified) {
              tech_pvt->buffer_overrun_notified = 1;
              tech_pvt->responseHandler(session, EVENT_BUFFER_OVERRUN, NULL);
            }
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "(%u) dropping packets!\n", 
              tech_pvt->id);
            pAudioPipe->binaryWritePtrResetToZero();

            frame.data = pAudioPipe->binaryWritePtr();
            frame.buflen = available = pAudioPipe->binarySpaceAvailable();
          }

          switch_status_t rv = switch_core_media_bug_read(bug, &frame, SWITCH_TRUE);
          if (rv != SWITCH_STATUS_SUCCESS) break;
          if (frame.datalen) {
            /* ★ Inspect BEFORE binaryWritePtrAdd: frame.data still points at the
             *   bytes we just read, and after the add the pointer has moved on. */
            noteFillFrame(session, tech_pvt, mod_af_frame_is_fill(frame.data, frame.datalen));
            pAudioPipe->binaryWritePtrAdd(frame.datalen);
            frame.buflen = available = pAudioPipe->binarySpaceAvailable();
            frame.data = pAudioPipe->binaryWritePtr();
          }
        }
      }
      else {
        uint8_t data[SWITCH_RECOMMENDED_BUFFER_SIZE];
        switch_frame_t frame;
        memset(&frame, 0, sizeof(frame));
        frame.data = data;
        frame.buflen = SWITCH_RECOMMENDED_BUFFER_SIZE;
        while (switch_core_media_bug_read(bug, &frame, SWITCH_TRUE) == SWITCH_STATUS_SUCCESS) {
          if (frame.datalen) {
            /* ★ Judge the frame as it arrived, before resampling: the resampler
             *   would smear 0xFF into values that are merely close to it. */
            noteFillFrame(session, tech_pvt, mod_af_frame_is_fill(frame.data, frame.datalen));
            spx_uint32_t out_len = available >> 1;  // space for samples which are 2 bytes
            spx_uint32_t in_len = frame.samples;

            speex_resampler_process_interleaved_int(tech_pvt->resampler, 
              (const spx_int16_t *) frame.data, 
              (spx_uint32_t *) &in_len, 
              (spx_int16_t *) ((char *) pAudioPipe->binaryWritePtr()),
              &out_len);

            if (out_len > 0) {
              // bytes written = num samples * 2 * num channels
              size_t bytes_written = out_len << tech_pvt->channels;
              pAudioPipe->binaryWritePtrAdd(bytes_written);
              available = pAudioPipe->binarySpaceAvailable();
            }
            if (available < pAudioPipe->binaryMinSpace()) {
              if (!tech_pvt->buffer_overrun_notified) {
                tech_pvt->buffer_overrun_notified = 1;
                switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "(%u) dropping packets!\n", 
                  tech_pvt->id);
                tech_pvt->responseHandler(session, EVENT_BUFFER_OVERRUN, NULL);
              }
              break;
            }
          }
        }
      }

      pAudioPipe->unlockAudioBuffer();
      switch_mutex_unlock(tech_pvt->mutex);
    }
    else {
      /* ════════════════════════════════════════════════════════════════════
       * ★★★ PR-2: this branch did not exist — the dropped frame was invisible
       * ════════════════════════════════════════════════════════════════════
       *
       * A10: a failed trylock skips one drain; the backlog is read on the next
       * successful acquire. So this is jitter, not lost audio. But at 50
       * frames/s per session it is exactly the signal PR-5/PR-6 would be trying
       * to improve, and there was no way to count it.
       *
       * ⚠ THROTTLED, and it has to be: 50 frames/s × N sessions firing an event
       *   per drop would push the contention into the event system itself —
       *   the measurement would become the problem.
       *
       * ★ The throttle counts FRAMES, not milliseconds: this is the audio
       *   callback, and a clock syscall here is exactly the kind of thing we are
       *   trying to measure. FRAME_DROP_REPORT_EVERY frames ≈ 10s at 50 fps.
       *
       * ★★ The event carries the **cumulative** count, not "1". A consumer that
       *   only sees increments cannot tell a throttled stream of reports from a
       *   burst, and cannot recover after a missed event. */
      tech_pvt->framesDroppedLock++;
      /* ★ 判据走 drop_throttle.hpp 的那个纯函数 —— 各写一遍的话单测验的是
       *   另一份实现（与 listExpiredMonths / tailPredicate 同一条理由）。 */
      if (mod_af_should_report_drop(tech_pvt->framesDroppedLock,
                                    tech_pvt->lastDropReportedAt,
                                    tech_pvt->dropReportEvery,
                                    FRAME_DROP_REPORT_EVERY)) {
        char buf[128];
        tech_pvt->lastDropReportedAt = tech_pvt->framesDroppedLock;
        snprintf(buf, sizeof(buf),
          "{\"frames_dropped_total\":%" PRIu64 ",\"reason\":\"trylock\"}",
          tech_pvt->framesDroppedLock);
        tech_pvt->responseHandler(session, EVENT_FRAME_DROPPED, buf);
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING,
          "(%u) mod_audio_fork: %" PRIu64 " frames skipped on mutex contention\n",
          tech_pvt->id, tech_pvt->framesDroppedLock);
      }
    }
    return SWITCH_TRUE;
  }

  /* Drain pending PCM from the playoutBuffer into the outgoing frame that
   * FreeSWITCH is about to send to the caller. If the buffer is empty, fill
   * the frame with silence so we never inject the upstream content (which
   * could mix our half-spoken TTS with whatever else is on the channel). */
  switch_bool_t dub_speech_frame(switch_media_bug_t *bug, private_t* tech_pvt) {
    if (!tech_pvt || !tech_pvt->bidirectional_audio_enable || !tech_pvt->playoutBuffer) {
      return SWITCH_TRUE;
    }

    switch_frame_t* rframe = switch_core_media_bug_get_write_replace_frame(bug);
    if (!rframe || rframe->datalen == 0) return SWITCH_TRUE;

    int16_t* out = reinterpret_cast<int16_t*>(rframe->data);
    const uint32_t samples_needed = rframe->samples;

    switch_mutex_lock(tech_pvt->mutex);
    PlayoutBuffer* buf = static_cast<PlayoutBuffer*>(tech_pvt->playoutBuffer);
    const size_t available = buf->size();
    const size_t to_copy = std::min(static_cast<size_t>(samples_needed), available);

    if (to_copy > 0) {
      std::copy_n(buf->begin(), to_copy, out);
      buf->erase_begin(to_copy);
    }
    if (to_copy < samples_needed) {
      std::fill(out + to_copy, out + samples_needed, 0);
    }

    // Advance the playback counter only by samples actually drained from the
    // buffer; padding silence doesn't count as "playing the queued audio".
    tech_pvt->playoutSamplesDrained += to_copy;

    // Fire any marks whose target sample index we just passed.
    if (tech_pvt->pendingMarks) {
      PendingMarkQueue* q = static_cast<PendingMarkQueue*>(tech_pvt->pendingMarks);
      while (!q->empty() && q->front().targetSample <= tech_pvt->playoutSamplesDrained) {
        sendMarkEvent(tech_pvt, q->front().name, "playout");
        q->pop_front();
      }
    }
    switch_mutex_unlock(tech_pvt->mutex);

    rframe->channels = 1;
    rframe->datalen = samples_needed * sizeof(int16_t);
    switch_core_media_bug_set_write_replace_frame(bug, rframe);
    return SWITCH_TRUE;
  }

}

