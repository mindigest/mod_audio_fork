/* 
 *
 * mod_audio_fork.c -- Freeswitch module for forking audio to remote server over websockets
 *
 */
#include "mod_audio_fork.h"
#include "lws_glue.h"

//static int mod_running = 0;

SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_audio_fork_shutdown);
SWITCH_MODULE_RUNTIME_FUNCTION(mod_audio_fork_runtime);
/* audio_fork_version — module-level API, no session needed.
 *
 * ★ Deliberately a SEPARATE api rather than a `uuid_audio_fork version`
 *   subcommand: that one requires a live session uuid, so on a fresh FS with no
 *   calls it cannot be asked at all — and "am I running the right module?" is
 *   exactly a startup-time question.
 *
 * Output is one JSON object on one line so a caller can parse it without
 * scraping. ⚠ Not a table: `fs_cli -x` output is what Go sees, and a
 * human-formatted table means a parser that breaks on cosmetics.
 */
#define VERSION_API_SYNTAX ""

SWITCH_STANDARD_API(fork_version_function)
{
	stream->write_function(stream,
		"{\"module\":\"mod_audio_fork\",\"version\":\"%s\","
		"\"capabilities\":{"
		"\"frame_drop_metrics\":%s,"
		"\"lockfree_writes\":%s,"
		"\"multithread_safe\":%s"
		"},"
		"\"service_threads\":%d,"
		"\"subprotocol\":\"%s\"}\n",
		MOD_AUDIO_FORK_VERSION,
		CAP_FRAME_DROP_METRICS ? "true" : "false",
		CAP_LOCKFREE_WRITES ? "true" : "false",
		CAP_MULTITHREAD_SAFE ? "true" : "false",
		fork_effective_threads(),
		fork_effective_subprotocol());
	return SWITCH_STATUS_SUCCESS;
}

SWITCH_MODULE_LOAD_FUNCTION(mod_audio_fork_load);

SWITCH_MODULE_DEFINITION(mod_audio_fork, mod_audio_fork_load, mod_audio_fork_shutdown, NULL /*mod_audio_fork_runtime*/);

static void responseHandler(switch_core_session_t* session, const char * eventName, char * json) {
	switch_event_t *event;

	switch_channel_t *channel = switch_core_session_get_channel(session);
	if (json) switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "responseHandler: sending event payload: %s.\n", json);
	switch_event_create_subclass(&event, SWITCH_EVENT_CUSTOM, eventName);
	switch_channel_event_set_data(channel, event);
	if (json) switch_event_add_body(event, "%s", json);
	switch_event_fire(&event);
}

static switch_bool_t capture_callback(switch_media_bug_t *bug, void *user_data, switch_abc_type_t type)
{
	(void)user_data;
	switch_core_session_t *session = switch_core_media_bug_get_session(bug);
	private_t* tech_pvt = (private_t *) switch_core_media_bug_get_user_data(bug);

	switch (type) {
	case SWITCH_ABC_TYPE_INIT:
		break;

	case SWITCH_ABC_TYPE_CLOSE:
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "Got SWITCH_ABC_TYPE_CLOSE for bug %s\n", tech_pvt->bugname);
		fork_session_cleanup(session, tech_pvt->bugname, NULL, 1);
		break;

	case SWITCH_ABC_TYPE_READ:
		return fork_frame(session, bug);

	case SWITCH_ABC_TYPE_WRITE_REPLACE:
		return dub_speech_frame(bug, tech_pvt);

	case SWITCH_ABC_TYPE_WRITE:
	default:
		break;
	}

	return SWITCH_TRUE;
}

static switch_status_t start_capture(switch_core_session_t *session,
        switch_media_bug_flag_t flags,
        char* host,
        unsigned int port,
        char* path,
        int sampling,
        int sslFlags,
        int bidirectional_audio_enable,
        int bidirectional_audio_stream,
        int bidirectional_audio_sample_rate,
        char* bugname,
        char* metadata)
{
	switch_channel_t *channel = switch_core_session_get_channel(session);
	switch_media_bug_t *bug;
	switch_status_t status;
	switch_codec_t* read_codec;

	void *pUserData = NULL;
  int channels = (flags & SMBF_STEREO) ? 2 : 1;

	switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO,
    "mod_audio_fork (%s): streaming %d sampling to %s path %s port %d tls: %s bidir: %s stream: %s stream_rate: %d.\n",
    bugname, sampling, host, path, port, sslFlags ? "yes" : "no",
    bidirectional_audio_enable ? "yes" : "no",
    bidirectional_audio_stream ? "yes" : "no",
    bidirectional_audio_sample_rate);

	if (switch_channel_get_private(channel, bugname)) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "mod_audio_fork: bug %s already attached!\n", bugname);
		return SWITCH_STATUS_FALSE;
	}

	read_codec = switch_core_session_get_read_codec(session);

	if (switch_channel_pre_answer(channel) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "mod_audio_fork: channel must have reached pre-answer status before calling start!\n");
		return SWITCH_STATUS_FALSE;
	}

	switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "calling fork_session_init.\n");
	if (SWITCH_STATUS_FALSE == fork_session_init(session, responseHandler, read_codec->implementation->actual_samples_per_second,
		host, port, path, sampling, sslFlags, channels, bugname, metadata,
		bidirectional_audio_enable, bidirectional_audio_stream, bidirectional_audio_sample_rate, &pUserData)) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Error initializing mod_audio_fork session.\n");
		return SWITCH_STATUS_FALSE;
	}
	switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "adding bug %s.\n", bugname);
	if ((status = switch_core_media_bug_add(session, bugname, NULL, capture_callback, pUserData, 0, flags, &bug)) != SWITCH_STATUS_SUCCESS) {
		return status;
	}
	switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "setting bug private data %s.\n", bugname);
	switch_channel_set_private(channel, bugname, bug);

	if (fork_session_connect(&pUserData) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Error mod_audio_fork session cannot connect.\n");
		return SWITCH_STATUS_FALSE;
	}

	switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "exiting start_capture.\n");
	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t do_stop(switch_core_session_t *session, char* bugname, char* text)
{
	switch_status_t status = SWITCH_STATUS_SUCCESS;

	if (text) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "mod_audio_fork (%s): stop w/ final text %s\n", bugname, text);
	}
	else {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "mod_audio_fork (%s): stop\n", bugname);
	}
	status = fork_session_cleanup(session, bugname, text, 0);

	return status;
}

static switch_status_t do_pauseresume(switch_core_session_t *session, char* bugname, int pause)
{
	switch_status_t status = SWITCH_STATUS_SUCCESS;

	switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "mod_audio_fork (%s): %s\n", bugname, pause ? "pause" : "resume");
	status = fork_session_pauseresume(session, bugname, pause);

	return status;
}

static switch_status_t do_stop_play(switch_core_session_t *session, char* bugname)
{
	switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "mod_audio_fork (%s): stop_play\n", bugname);
	return fork_session_stop_play(session, bugname);
}

static switch_status_t do_graceful_shutdown(switch_core_session_t *session, char* bugname)
{
	switch_status_t status = SWITCH_STATUS_SUCCESS;

	switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "mod_audio_fork (%s): do_graceful_shutdown \n", bugname);
	status = fork_session_graceful_shutdown(session, bugname);

	return status;
}

static switch_status_t send_text(switch_core_session_t *session, char* bugname, char* text) {
	switch_status_t status = SWITCH_STATUS_FALSE;

	switch_channel_t *channel = switch_core_session_get_channel(session);
	switch_media_bug_t *bug = switch_channel_get_private(channel, bugname);

  if (bug) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "mod_audio_fork (%s): sending text: %s.\n", bugname, text);
    status = fork_session_send_text(session, bugname, text);
  }
  else {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "mod_audio_fork (%s): no bug, failed sending text: %s.\n", bugname, text);
  }
  return status;
}

/* ⚠⚠ bidirectionalAudio_enabled has a consequence that is easy to miss:
 *
 *   It adds SMBF_WRITE_REPLACE, and dub_speech_frame() then replaces EVERY
 *   outgoing frame — filling with zeroes whenever our playout buffer is empty
 *   (lws_glue.cpp). So from the moment it is enabled, the caller hears ONLY
 *   what the WebSocket server sends: any dialplan playback(), any bridged
 *   audio, any moh is silenced for the rest of the call.
 *
 *   That is intentional — mixing half-spoken TTS with whatever else is on the
 *   channel is worse — but it makes `true` here a much bigger switch than the
 *   name suggests. See the README section "Bidirectional audio takes over the
 *   channel".
 *
 * ★ The converse also bites: WRITE_REPLACE only fires while something is
 *   actually writing to the channel. On an idle or parked leg nothing writes,
 *   dub_speech_frame is never called, and the playout buffer fills without ever
 *   draining. If you are testing playback, give the leg a writer — e.g.
 *   playback(silence_stream://3600000,0).
 */
#define FORK_API_SYNTAX "<uuid> [start | stop | send_text | pause | resume | stop_play | graceful-shutdown ] [wss-url | path] [mono | mixed | stereo] [8000 | 16000 | 24000 | 32000 | 64000] [bugname] [metadata] [bidirectionalAudio_enabled] [bidirectionalAudio_stream_enabled] [bidirectionalAudio_stream_samplerate]"
SWITCH_STANDARD_API(fork_function)
{
	char *mycmd = NULL, *argv[10] = { 0 };
	int argc = 0;
	switch_status_t status = SWITCH_STATUS_FALSE;
  char *bugname = MY_BUG_NAME;

	if (!zstr(cmd) && (mycmd = strdup(cmd))) {
		argc = switch_separate_string(mycmd, ' ', argv, (sizeof(argv) / sizeof(argv[0])));
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "mod_audio_fork cmd: %s\n", cmd);
	}


	if (zstr(cmd) || argc < 2 ||
		(0 == strcmp(argv[1], "start") && argc < 4)) {

		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
			"Error with command '%s' (argv[0]='%s' argv[1]='%s').\n",
			cmd ? cmd : "(null)",
			argv[0] ? argv[0] : "(null)",
			argv[1] ? argv[1] : "(null)");
		stream->write_function(stream, "-USAGE: %s\n", FORK_API_SYNTAX);
		goto done;
	} else {
		switch_core_session_t *lsession = NULL;

		if ((lsession = switch_core_session_locate(argv[0]))) {
			if (!strcasecmp(argv[1], "stop")) {
        char * text = NULL;
        if (argc > 3) {
          bugname = argv[2];
          text = argv[3];
        }
        else if (argc > 2) {
          if (argv[2][0] == '{' || argv[2][0] == '[') text = argv[2];
          else bugname = argv[2];
        }
				status = do_stop(lsession, bugname, text);
      }
			else if (!strcasecmp(argv[1], "stop_play")) {
        if (argc > 2) bugname = argv[2];
				status = do_stop_play(lsession, bugname);
      }
			else if (!strcasecmp(argv[1], "pause")) {
        if (argc > 2) bugname = argv[2];
				status = do_pauseresume(lsession, bugname, 1);
      }
			else if (!strcasecmp(argv[1], "resume")) {
        if (argc > 2) bugname = argv[2];
				status = do_pauseresume(lsession, bugname, 0);
      }
			else if (!strcasecmp(argv[1], "graceful-shutdown")) {
        if (argc > 2) bugname = argv[2];
				status = do_graceful_shutdown(lsession, bugname);
      }
      else if (!strcasecmp(argv[1], "send_text")) {
        char * text = 0;
        if (argc < 3) {
          switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "send_text requires an argument specifying text to send\n");
          switch_core_session_rwunlock(lsession);
          goto done;
        }
        if (argc > 3) {
          bugname = argv[2];
          text = argv[3];
        }
        else {
          if (argv[2][0] == '{' || argv[2][0] == '[') text = argv[2];
          else bugname = argv[2];
        }
        status = send_text(lsession, bugname, text);
      }
      else if (!strcasecmp(argv[1], "start")) {
				switch_channel_t *channel = switch_core_session_get_channel(lsession);
        /* ★ Initialised at declaration: the third layer. parse_ws_uri zeroes
         * them on failure and the guard below stops on failure — but a stack
         * array whose first byte is never written is a landmine regardless of
         * who is supposed to step around it. */
        char host[MAX_WS_URL_LEN] = {0}, path[MAX_PATH_LEN] = {0};
        unsigned int port;
        int sslFlags;
        int sampling = 8000;
      	switch_media_bug_flag_t flags = SMBF_READ_STREAM ;
        char *metadata = NULL;
        int bidirectional_audio_enable = 0;
        int bidirectional_audio_stream = 0;
        int bidirectional_audio_sample_rate = 0;
        /* Set by any argument-validation branch below. ★ We keep going to the
         * common rwunlock + status write instead of jumping past them. */
        int bad_args = 0;
        // Three layered argument shapes for backward compatibility:
        //   argc > 9  : ... bugname metadata bidir stream rate
        //   argc > 7  : ... bugname metadata bidir
        //   argc > 6  : ... bugname metadata
        //   argc > 5  : ... (bugname xor metadata, disambiguated by '{')
        if (argc > 9) {
          if (argv[5][0] != '\0') bugname = argv[5];
          if (argv[6][0] != '\0') metadata = argv[6];
          bidirectional_audio_enable = !strcmp(argv[7], "true") ? 1 : 0;
          bidirectional_audio_stream = !strcmp(argv[8], "true") ? 1 : 0;
          bidirectional_audio_sample_rate = atoi(argv[9]);
        }
        else if (argc > 7) {
          if (argv[5][0] != '\0') bugname = argv[5];
          if (argv[6][0] != '\0') metadata = argv[6];
          bidirectional_audio_enable = !strcmp(argv[7], "true") ? 1 : 0;
        }
        else if (argc > 6) {
          bugname = argv[5];
          metadata = argv[6];
        }
        else if (argc > 5) {
          if (argv[5][0] == '{' || argv[5][0] == '[') metadata = argv[5];
          else bugname = argv[5];
        }
        if (bidirectional_audio_enable) {
          flags |= SMBF_WRITE_REPLACE;
        }
        if (0 == strcmp(argv[3], "mixed")) {
          flags |= SMBF_WRITE_STREAM ;
        }
        else if (0 == strcmp(argv[3], "stereo")) {
          flags |= SMBF_WRITE_STREAM ;
          flags |= SMBF_STEREO;
        }
        else if(0 != strcmp(argv[3], "mono")) {
          switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "invalid mix type: %s, must be mono, mixed, or stereo\n", argv[3]);
          /* ⚠ This used to `switch_core_session_rwunlock(lsession); goto done;`
           * — and `done:` sits AFTER the +OK/-ERR write, so the caller got an
           * empty response for an invalid mix type. Same defect as the URI
           * branch below; found while fixing that one. */
          bad_args = 1;
        }
        if (0 == strcmp(argv[4], "16k")) {
          sampling = 16000;
        }
        else if (0 == strcmp(argv[4], "8k")) {
          sampling = 8000;
        }
				else {
					sampling = atoi(argv[4]);
				}
        /* ════════════════════════════════════════════════════════════════
         * ★★★ Both of these used to log and then fall through
         * ════════════════════════════════════════════════════════════════
         *
         * The original was:
         *
         *     if (!parse_ws_uri(...)) { log("invalid websocket uri"); }
         *     else if (sampling % 8000 != 0) { log("invalid sample rate"); }
         *     status = start_capture(..., host, port, path, sampling, ...);
         *
         * There was no `else` on that last statement, so start_capture ran
         * unconditionally:
         *
         *  · on a parse failure, `host` and `path` are the fixed-size stack
         *    arrays declared above and **not one byte of them had been
         *    written** ⇒ uninitialised stack memory used as the target of a
         *    network connection.
         *  · on an illegal sample rate, we logged the error and then used the
         *    illegal rate anyway.
         *
         * ⚠ Both failures are quiet: the log line scrolls past and the module
         *   goes on to do something. That is docs/CONSTRAINTS.md A4/A5.
         *
         * ★ parse_ws_uri now also zeroes host/path on every failure path, so
         *   even a future caller that forgets this guard cannot read junk —
         *   two layers, because this one is the layer that already failed once.
         */
        /* ★★★ else-if chain, NOT `goto done` — see the note above `done:`.
         *
         * `status` is SWITCH_STATUS_FALSE at declaration, so simply not calling
         * start_capture is what produces `-ERR Operation Failed`. */
        if (bad_args) {
          /* already logged above; fall through to the -ERR write */
        }
        else if (!parse_ws_uri(channel, argv[2], &host[0], &path[0], &port, &sslFlags)) {
          switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
            "invalid websocket uri: %s — not starting capture\n", argv[2]);
        }
        else if (sampling % 8000 != 0) {
          switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
            "invalid sample rate: %s (must be a multiple of 8000) — not starting capture\n", argv[4]);
        }
        else {
          status = start_capture(lsession, flags, host, port, path, sampling, sslFlags,
            bidirectional_audio_enable, bidirectional_audio_stream, bidirectional_audio_sample_rate,
            bugname, metadata);
        }
			}
      else {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "unsupported mod_audio_fork cmd: %s\n", argv[1]);
      }
			switch_core_session_rwunlock(lsession);
		}
		else {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Error locating session %s\n", argv[0]);
		}
	}

	if (status == SWITCH_STATUS_SUCCESS) {
		stream->write_function(stream, "+OK Success\n");
	} else {
		stream->write_function(stream, "-ERR Operation Failed\n");
	}

  /* ════════════════════════════════════════════════════════════════════════
   * ★★★ `done:` is AFTER the +OK/-ERR write — jumping here skips the response
   * ════════════════════════════════════════════════════════════════════════
   *
   * It exists only to free `mycmd` on the early argument-count failures, which
   * happen before `stream` has anything to say.
   *
   * ⚠ Any validation failure that happens INSIDE the command handlers must NOT
   *   `goto done`: the caller then gets an empty body, which the API layer
   *   renders as a success. Leave `status` at SWITCH_STATUS_FALSE and fall
   *   through to the write instead.
   *
   * ★ Both the invalid-mix-type branch and (briefly) the new URI guard got this
   *   wrong. It is an easy mistake precisely because `goto done` is right there
   *   in the same function and looks like the local idiom.
   */
  done:

	switch_safe_free(mycmd);
	return SWITCH_STATUS_SUCCESS;
}


SWITCH_MODULE_LOAD_FUNCTION(mod_audio_fork_load)
{
	switch_api_interface_t *api_interface;

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_audio_fork API loading..\n");

	/* connect my internal structure to the blank pointer passed to me */
	*module_interface = switch_loadable_module_create_module_interface(pool, modname);

	/* create/register custom event message types */
	if (switch_event_reserve_subclass(EVENT_TRANSCRIPTION) != SWITCH_STATUS_SUCCESS ||
    switch_event_reserve_subclass(EVENT_TRANSFER) != SWITCH_STATUS_SUCCESS ||
    switch_event_reserve_subclass(EVENT_PLAY_AUDIO) != SWITCH_STATUS_SUCCESS ||
    switch_event_reserve_subclass(EVENT_KILL_AUDIO) != SWITCH_STATUS_SUCCESS ||
    switch_event_reserve_subclass(EVENT_ERROR) != SWITCH_STATUS_SUCCESS ||
    switch_event_reserve_subclass(EVENT_DISCONNECT) != SWITCH_STATUS_SUCCESS ||
    switch_event_reserve_subclass(EVENT_FRAME_DROPPED) != SWITCH_STATUS_SUCCESS ||
    switch_event_reserve_subclass(EVENT_MEDIA_SILENT) != SWITCH_STATUS_SUCCESS ||
    /* ════════════════════════════════════════════════════════════════════
     * ★★★ PR-4: these four were DEFINED in the header and never reserved
     * ════════════════════════════════════════════════════════════════════
     *
     * mod_audio_fork.h declared connect / connect_failed / buffer_overrun /
     * json since forever, and responseHandler() fires all four — but
     * mod_load only reserved six subclasses, so these were never registered.
     *
     * ⚠ The consequence is not an error anywhere. switch_event_create_subclass
     *   on an unreserved subclass still fires; what you lose is
     *   `/event plain CUSTOM mod_audio_fork::connect` finding it, i.e.
     *   **a subscriber gets nothing and no side reports a problem**.
     *   That is docs/CONSTRAINTS.md A11/A12.
     *
     * ★ And it mattered concretely: `internal/telephony/esl/client.go`
     *   subscribed to only the same six, so even a fixed module would have
     *   been talking to a deaf listener. Both sides land in this PR.
     */
    switch_event_reserve_subclass(EVENT_CONNECT_SUCCESS) != SWITCH_STATUS_SUCCESS ||
    switch_event_reserve_subclass(EVENT_CONNECT_FAIL) != SWITCH_STATUS_SUCCESS ||
    switch_event_reserve_subclass(EVENT_BUFFER_OVERRUN) != SWITCH_STATUS_SUCCESS ||
    switch_event_reserve_subclass(EVENT_JSON) != SWITCH_STATUS_SUCCESS) {

		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Couldn't register an event subclass for mod_audio_fork API.\n");
		return SWITCH_STATUS_TERM;
	}

	SWITCH_ADD_API(api_interface, "uuid_audio_fork", "audio_fork API", fork_function, FORK_API_SYNTAX);
	/* PR-3: see fork_version_function. Registered even if everything else fails
	 * to initialise — the whole point is being askable when things are wrong. */
	SWITCH_ADD_API(api_interface, "audio_fork_version",
		"mod_audio_fork version + capabilities (JSON)", fork_version_function, VERSION_API_SYNTAX);
	switch_console_set_complete("add audio_fork_version");
	switch_console_set_complete("add uuid_audio_fork start wss-url metadata");
	switch_console_set_complete("add uuid_audio_fork start wss-url");
	switch_console_set_complete("add uuid_audio_fork stop");

	fork_init();

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_audio_fork API successfully loaded\n");

	/* indicate that the module should continue to be loaded */
  //mod_running = 1;
	return SWITCH_STATUS_SUCCESS;
}

/*
  Called when the system shuts down
  Macro expands to: switch_status_t mod_audio_fork_shutdown() */
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_audio_fork_shutdown)
{
	fork_cleanup();
  //mod_running = 0;
	switch_event_free_subclass(EVENT_TRANSCRIPTION);
	switch_event_free_subclass(EVENT_TRANSFER);
	switch_event_free_subclass(EVENT_PLAY_AUDIO);
	switch_event_free_subclass(EVENT_KILL_AUDIO);
	switch_event_free_subclass(EVENT_DISCONNECT);
	switch_event_free_subclass(EVENT_ERROR);
	switch_event_free_subclass(EVENT_FRAME_DROPPED);
	switch_event_free_subclass(EVENT_MEDIA_SILENT);
	/* ★ 必须与 reserve 一一对应：漏 free 的症状是**在同一个进程里 reload 之后
	 *   load 失败**（"module load file routine returned an error"），而那个报错
	 *   不提事件子类。⚠ 实测撞到过一次，当时以为是 .so 坏了。 */
	switch_event_free_subclass(EVENT_CONNECT_SUCCESS);
	switch_event_free_subclass(EVENT_CONNECT_FAIL);
	switch_event_free_subclass(EVENT_BUFFER_OVERRUN);
	switch_event_free_subclass(EVENT_JSON);

	return SWITCH_STATUS_SUCCESS;
}

/*
  If it exists, this is called in it's own thread when the module-load completes
  If it returns anything but SWITCH_STATUS_TERM it will be called again automatically
  Macro expands to: switch_status_t mod_audio_fork_runtime()
*/
/*
SWITCH_MODULE_RUNTIME_FUNCTION(mod_audio_fork_runtime)
{
  fork_service_threads(&mod_running);
	return SWITCH_STATUS_TERM;
}
*/
