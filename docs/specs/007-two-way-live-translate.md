# 007 — Two-way captions with gemini-3.5-live-translate-preview

Status: implemented, unit-tested; not yet verified against the live API.

## 1. Goal

Show captions in both directions at once, e.g. Japanese speech → English
captions and English speech → Japanese captions, from one microphone, using
Google's dedicated speech translation model `gemini-3.5-live-translate-preview`.

## 2. Non-goals

- Playing or recording the translated speech the model returns (it is dropped).
- More than two languages, or more than one two-way pair.
- Replacing the one-way engine: it stays the default and is unchanged.

## 3. API facts (Gemini docs, Live translation)

- Endpoint: the usual Live API WebSocket (`v1beta ... BidiGenerateContent`).
- Setup: `setup.generationConfig` carries `responseModalities: ["AUDIO"]`
  (the only allowed modality) and `translationConfig: {targetLanguageCode,
  echoTargetLanguage}`; `inputAudioTranscription: {}` and
  `outputAudioTranscription: {}` go on `setup` itself. The docs' raw-JSON
  sample nests the transcriptions in `generationConfig`, but the server
  rejects that (close 1007, "Unknown name ... at 'setup.generation_config'");
  the placement here follows the google-genai SDK's converter.
- Source language is auto-detected; one target language per session.
- `echoTargetLanguage: false` (the default) keeps the model silent when the
  speech is already in the target language.
- Input: 16 kHz mono S16LE PCM, 100 ms chunks (same as the one-way engine).
- Text arrives as `serverContent.inputTranscription` /
  `serverContent.outputTranscription` (`text`, `languageCode`).

## 4. Design

### 4.1 Protocol (`live-translate-protocol.*`, pure)

Builds the setup above and parses server messages into input/output text
deltas, `turnComplete`, `setupComplete`, `goAway` and `error`.

### 4.2 Display (`live-caption-buffer.*`, pure)

One buffer per text source. Deltas are concatenated verbatim; whitespace at a
line start is dropped; `turnComplete` makes the next delta start a new line.
Rendering is `wrap_tail()` to Max Characters per Line × Caption Lines (CJK = 2).
The text is cleared Caption Hold seconds after the last non-blank delta.

### 4.3 Session (`LiveTranslateSession`)

One WebSocket per target language with `echoTargetLanguage: false`; reconnect
with exponential backoff (1 s → 30 s); rotation at 9 minutes or on `goAway`.
Per-message deflate is off (base64 PCM does not compress; it was a suspect for
a sporadic mid-session close 1007 "Invalid JSON payload").

Failure policy, for events that must not lose captions:
- Close 1007/1008 or an `error` message **before the server ever accepted a
  setup** with the current key/target (bad key, unknown model) stops the
  session; the status shows the reason.
- **After one accepted setup**, every failure, including 1007/1008 and quota
  errors, reconnects with backoff. The status reads `Reconnecting... (reason)`.
  The backoff resets only after a connection whose setup was accepted, so a
  quota rejection does not retry every second.
- Once a minute while connected, each session logs
  `stats: input_deltas output_deltas last_output_ms_ago`.

### 4.6 Watchdogs

Observed live (2026-10-07): after a routine 9-minute rotation, both new
connections opened, received audio, and then sent nothing at all (no close,
no error) for minutes.

- **Deaf session:** while the mic is audible (a chunk at or above the idle
  threshold within the last 2 s), a connected session that has sent no input
  or output transcription for 45 s reconnects (no backoff). Music without
  speech can trigger it; the reconnect costs well under a second and the
  queued audio is kept.
- **Stall:** every utterance is in language A or B, so one session should
  translate it. If input transcription keeps arriving (gaps ≤ 10 s) for 30 s
  while neither session produced output, both reconnect, at most once a minute.

Stats lines also count all server messages (audio included), and
`setupComplete` is logged per connection, to tell a hung server from a
rejected setup.

### 4.4 Two-way owner (`TwoWayTranslate`)

Owns session A (target = Language A) and session B (target = Language B) and
feeds both the same audio. Auto-pause (spec 004) is evaluated once for both:
waiting for sound at start, idle timeout, and "only while output active".
The optional source transcript comes from session A only, so it is not
duplicated.

### 4.5 Filter settings

| key | type | meaning |
| --- | --- | --- |
| `engine` | string | `transcribe` (default, one-way) or `live_two_way` |
| `two_way_lang_a` | string | Language A, default `ja` |
| `two_way_lang_b` | string | Language B, default `en` |
| `two_way_caption_a_text_source` | string | text source for captions in Language A |
| `two_way_caption_b_text_source` | string | text source for captions in Language B |

Shared with the one-way engine: `api_key`, `caption_source_text_source`,
`caption_max_lines`, `caption_max_chars_per_line`, `caption_hold_seconds`,
`idle_timeout_seconds`, `idle_threshold_dbfs`, `only_while_output_active`.
The properties panel shows only the selected engine's settings.

## 5. Cost

Both sessions receive all audio, so audio input is billed twice compared with
one session. Auto-pause applies to both.

## 6. To verify live

1. The setup is accepted with transcription configs on `setup` (the
   `generationConfig` placement was rejected in the first live test).
2. With `echoTargetLanguage: false`, a session emits no `outputTranscription`
   for speech already in its target language.
3. Session A still emits `inputTranscription` for Language A speech.
4. Transcription deltas carry their own spacing (English) and arrive in order.
5. The session length limit (rotation is at 9 minutes as a guess).
