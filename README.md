# mod_audio_fork

A Freeswitch module that attaches a bug to a media server endpoint and streams L16 audio via websockets to a remote server.  This module also supports receiving media from the server to play back to the caller, enabling the creation of full-fledged IVR or dialog-type applications.

## Build & install

Out-of-tree CMake build against an installed FreeSWITCH. Full details and
per-distro dependency hints are in [BUILD.md](BUILD.md).

```bash
# 1. Install dependencies (Debian/Ubuntu shown; see BUILD.md for others)
sudo apt-get install -y cmake build-essential pkg-config \
    libwebsockets-dev libspeexdsp-dev libboost-dev freeswitch-dev

# 2. Build mod_audio_fork.so
./build.sh build
#   -> build/mod_audio_fork.so

# 3. Install into FreeSWITCH's module directory
sudo ./build.sh install
#   -> /usr/local/freeswitch/mod/mod_audio_fork.so

# 4. Tell FreeSWITCH to load it (add to modules.conf.xml)
#    <load module="mod_audio_fork"/>
fs_cli -x "reload mod_audio_fork"
```

Override the FreeSWITCH location with `FREESWITCH_INCLUDE_DIR=...
FREESWITCH_LIBRARY=...` (passed straight through to CMake), or the
install target with `FREESWITCH_MOD_DIR=...`. See [BUILD.md](BUILD.md)
for non-standard layouts and the manual `cmake` invocation.

#### Environment variables
- MOD_AUDIO_FORK_SUBPROTOCOL_NAME - optional, name of the [websocket sub-protocol](https://tools.ietf.org/html/rfc6455#section-1.9) to advertise; defaults to "audio.drachtio.org"
- MOD_AUDIO_FORK_SERVICE_THREADS - optional, number of libwebsocket service threads to create; these threads handling sending all messages for all sessions.  Defaults to 1, but can be set to as many as 5.
- MOD_AUDIO_FORK_BUFFER_SECS - optional, size of the outbound audio buffer in seconds (1–5); defaults to 2.
- MOD_AUDIO_FORK_TCP_KEEPALIVE_SECS - optional, TCP keep-alive idle interval on the WebSocket socket; defaults to 55 seconds.

## API

### Commands
The freeswitch module exposes the following API commands:

```
uuid_audio_fork <uuid> start <wss-url> <mix-type> <sampling-rate> <metadata>
```
Attaches media bug and starts streaming audio stream to the back-end server.  Audio is streamed in linear 16 format (16-bit PCM encoding) with either one or two channels depending on the mix-type requested.
- `uuid` - unique identifier of Freeswitch channel
- `wss-url` - websocket url to connect and stream audio to
- `mix-type` - choice of 
  - "mono" - single channel containing caller's audio
  - "mixed" - single channel containing both caller and callee audio
  - "stereo" - two channels with caller audio in one and callee audio in the other.
- `sampling-rate` - choice of
  - "8k" = 8000 Hz sample rate will be generated
  - "16k" = 16000 Hz sample rate will be generated
- `metadata` - a text frame of arbitrary data to send to the back-end server immediately upon connecting.  Once this text frame has been sent, the incoming audio will be sent in binary frames to the server.

```
uuid_audio_fork <uuid> send_text <metadata>
```
Send a text frame of arbitrary data to the remote server (e.g. this can be used to notify of DTMF events).

```
uuid_audio_fork <uuid> stop <metadata>
```
Closes websocket connection and detaches media bug, optionally sending a final text frame over the websocket connection before closing.

### Bidirectional audio takes over the channel

Passing `true` for `bidirectionalAudio_enabled` is a bigger switch than the name
suggests, in both directions.

**It silences everything else on the channel.** The flag adds
`SMBF_WRITE_REPLACE`, and `dub_speech_frame()` then replaces *every* outgoing
frame — filling with zeroes whenever the playout buffer is empty. From that
moment the caller hears only what your WebSocket server sends: dialplan
`playback()`, bridged audio and music-on-hold are all gone for the rest of the
call. This is deliberate (mixing half-spoken TTS with whatever else is on the
channel is worse), but it is not obvious from the parameter name.

**It only works on a leg that something is writing to.** `WRITE_REPLACE` fires
from `switch_core_session_write_frame()`. On an idle or parked leg nothing
writes, so `dub_speech_frame()` is never called and the playout buffer fills
without ever draining — the symptom is "I send `playAudio`, the buffer grows,
the caller hears nothing". If you are testing playback, give the leg a writer:

```xml
<action application="playback" data="silence_stream://3600000,0"/>
```

### Events
An optional feature of this module is that it can receive JSON text frames from the server and generate associated events to an application.  The format of the JSON text frames and the associated events are described below.

#### audio
##### server JSON message
The server can provide audio content to be played back to the caller by sending a JSON text frame like this:
```json
{
	"type": "playAudio",
	"data": {
		"audioContentType": "raw",
		"sampleRate": 8000,
		"audioContent": "base64 encoded raw audio..",
		"textContent": "Hi there!  How can we help?"
	}
}
```
The `audioContentType` value can be either `wave` or `raw`.  If the latter, then `sampleRate` must be specified.  The audio content itself is supplied as a base64 encoded string.  The `textContent` attribute can optionally contain the text of the prompt.  This allows an application to choose whether to play the raw audio or to use its own text-to-speech to play the text prompt.

Note that the module does _not_ directly play out the raw audio.  Instead, it writes it to a temporary file and provides the path to the file in the event generated.  It is left to the application to play out this file if it wishes to do so.
##### Freeswitch event generated
**Name**: mod_audio_fork::play_audio
**Body**: JSON string
```
{
  "audioContentType": "raw",
  "sampleRate": 8000,
  "textContent": "Hi there!  How can we help?",
  "file": "/tmp/7dd5e34e-5db4-4edb-a166-757e5d29b941_2.tmp.r8"
}
```
Note the audioContent attribute has been replaced with the path to the file containing the audio.  This temporary file will be removed when the Freeswitch session ends.
#### killAudio
##### server JSON message
The server can provide a request to kill the current audio playback:
```json
{
	"type": "killAudio",
}
```
Any current audio being played to the caller will be immediately stopped.  The event sent to the application is for information purposes only.

##### Freeswitch event generated
**Name**: mod_audio_fork::kill_audio
**Body**: JSON string - the data attribute from the server message


#### transcription
##### server JSON message
The server can optionally provide transcriptions to the application in real-time:
```json
{
	"type": "transcription",
	"data": {
    
	}
}
```
The transcription data can be any JSON object; for instance, a server may choose to return a transcript and an associated confidence level.  Whatever is provided as the `data` attribute will be attached to the generated event.

##### Freeswitch event generated
**Name**: mod_audio_fork::transcription
**Body**: JSON string - the data attribute from the server message

#### transfer
##### server JSON message
The server can optionally provide a request to transfer the call:
```json
{
	"type": "transfer",
	"data": {
    
	}
}
```
The transfer data can be any JSON object and is left for the application to determine how to handle it and accomplish the call transfer.  Whatever is provided as the `data` attribute will be attached to the generated event.

##### Freeswitch event generated
**Name**: mod_audio_fork::transfer
**Body**: JSON string - the data attribute from the server message

#### disconnect
##### server JSON message
The server can optionally request to disconnect the caller:
```json
{
	"type": "disconnect"
}
```
Note that the module _does not_ close the Freeswitch channel when a disconnect request is received.  It is left for the application to determine whether to tear down the call.

##### Freeswitch event generated
**Name**: mod_audio_fork::disconnect
**Body**: none

#### error
##### server JSON message
The server can optionally report an error of some kind.  
```json
{
	"type": "error",
	"data": {
    
	}
}
```
The error data can be any JSON object and is left for the application to the application to determine what, if any, action should be taken in response to an error..  Whatever is provided as the `data` attribute will be attached to the generated event.

##### Freeswitch event generated
**Name**: mod_audio_fork::error
**Body**: JSON string - the data attribute from the server message

#### Module-originated events

The events above are all *responses* — the server sends a JSON message and the
module raises a FreeSWITCH event. The two below have **no server message**: the
module raises them on its own when it notices something the caller cannot see
from the outside. Both are throttled (they describe conditions that, once true,
tend to stay true for thousands of frames).

Negotiate on the capability bits from `audio_fork_version` rather than on the
module version — see [Capabilities](#capabilities).

##### mod_audio_fork::media_silent

Capability bit: `media_silent` (0.3.0+)

The inbound stream is FreeSWITCH's no-media fill, not audio. FreeSWITCH does
not hand us silence and does not hand us an error — it hands us a frame
`memset` to `0xFF`, and the frame carries `SFF_CNG`, so FS *knows* it is not
audio. Without this event the condition is invisible from outside: the call
answers, the CDR bills, and the transcript is simply empty.

```json
{"consecutive_fill_frames":150,"consecutive_ms":3000,"total_fill_frames":150}
```

★ `consecutive_*` reset when real audio resumes; `total_fill_frames` does not.
A call that ends with a large `total` and a small `consecutive` had gaps; one
where they are equal and large never received anything at all.

##### mod_audio_fork::frame_dropped

Capability bit: `frame_drop_metrics` (0.2.0+)

A frame was skipped because `trylock` on the write path lost. Counted rather
than logged-and-forgotten so that "we dropped audio" stops being invisible.

```json
{"frames_dropped_total":3,"reason":"trylock"}
```

## Capabilities

`audio_fork_version` returns the module's identity as one line of JSON, so a
consumer can negotiate instead of assuming:

```console
$ fs_cli -x 'audio_fork_version'
{"module":"mod_audio_fork","version":"0.3.0","capabilities":{"frame_drop_metrics":true,"media_silent":true,"lockfree_writes":false,"multithread_safe":false},"service_threads":1,"subprotocol":"audio.drachtio.org"}
```

★★★ **Ask for the bit, not the version.** The version string is bumped by hand
at release time; between releases several commits can share one version. In
0.2.0 that is exactly what happened — five commits, including a
use-after-destroy mutex and three data races, all reporting `"0.2.0"` — and a
deployment running the pre-fix build was indistinguishable from one running the
fixes. The bits move with the feature; the string moves with the release.

★★ A bit reported `false` is the honest answer for a feature that is not
built. `lockfree_writes` and `multithread_safe` are hard-wired to 0 and stay
declared on purpose: they are the negotiation surface for the day someone needs
to scale up. **A bit that lies is worse than a bit that is absent** — a
consumer that sees a stale `true` skips a workaround it still needs.

## Tests

See [tests/README.md](tests/README.md) for the smoke + protocol test
suite. TL;DR:

```bash
./tests/smoke.sh           # build / load / API surface
./tests/protocol_test.sh   # end-to-end WS protocol against a mock peer
```

## License

Released under the MIT License — see [LICENSE](LICENSE) for the full text.

Originally derived from the `mod_audio_fork` module in
[drachtio/drachtio-freeswitch-modules](https://github.com/drachtio/drachtio-freeswitch-modules)
and maintained independently here under the same MIT terms.
Original copyright remains with Drachtio Communications Services, LLC.
