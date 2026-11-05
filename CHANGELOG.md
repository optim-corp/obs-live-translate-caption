# Changelog

All notable user-facing changes to this project are documented here. The
project follows [Semantic Versioning](https://semver.org/).

## [Unreleased]

### Added

- Preview two-way captions using two `gemini-3.5-live-translate-preview`
  sessions, with separate text sources for Language A and Language B.
- Session rotation and watchdogs for stalled two-way transcription or translation.
- Korean two-way settings and usage instructions.
- An OBS-independent unit-test configuration for manual validation.

### Changed

- Point repository and support links to `optim-corp/obs-live-translate-caption`,
  while preserving upstream acknowledgments and copyright notices.
- Remove GitHub Actions build/test and release workflows. Company builds,
  tests, and release uploads are performed manually; Actions is disabled.

### Validation

- All 526 CTest cases and native plugin builds passed on Windows, macOS, and
  Linux in [company CI](https://github.com/optim-corp/obs-live-translate-caption/actions/runs/41975613559).
- The complete two-way live OBS verification checklist remains pending.

## [1.1.0] - 2026-09-12

### Added

- Translate stable parts of a long utterance before finalization, with an
  option to keep final-only translation.
- Split long translations into ordered pages and append them to a bounded
  rolling caption window.
- Detect suspected untranslated output locally and retry it once after the
  first caption is visible.
- Expose `Translate while speaking` and `Retry suspected untranslated captions
  once` settings, both enabled by default.

### Changed

- Match SMART transcription revisions by internal utterance, segment and
  revision identifiers so stale results cannot overwrite current captions.
- Join segments from the same finalized utterance on one line when they fit,
  and restart the hold timer only after a successful visible update.
- Use one short translation instruction across target languages and show
  endonyms while preserving BCP-47 region codes.
- Prefer translated captions and disable source-transcript output when both
  outputs point to the same OBS text source.

### Fixed

- Preserve ordered display when translation responses complete out of order.
- Keep visible captions during pending revisions or failed quality retries,
  and prevent expired or retired results from reappearing.
- Bound queued work, retained source text and retry concurrency during long or
  rapidly revised utterances.

### Validation

- Windows plugin build and all 488 CTest cases pass.
- A two-hour Windows OBS smoke run found no major issue. Quantitative live-model
  quality, API latency and OBS load comparisons remain pending.
- macOS and Linux packages are built by CI but have not been verified on those
  platforms.

## [1.0.1] - 2026-09-05

### Fixed

- Detect long idle periods even when an OBS audio source stops delivering
  callbacks.
- Preserve idle timing across WebSocket rotations and reconnects, and preserve
  buffered first words while a connection opens.
- Log pause-reason transitions and periodic idle diagnostics.

## [1.0.0] - 2026-09-05

### Added

- Initial captions-only release for Windows, macOS and Linux.
- Live Gemini transcription, per-sentence translation, OBS text-source output,
  configurable wrapping and hold time, reconnect handling and idle auto-pause.

[Unreleased]: https://github.com/optim-corp/obs-live-translate-caption/commits/main
[1.1.0]: https://github.com/mintonnee/obs-live-translate-caption/compare/v1.0.1...v1.1.0
[1.0.1]: https://github.com/mintonnee/obs-live-translate-caption/compare/v1.0.0...v1.0.1
[1.0.0]: https://github.com/mintonnee/obs-live-translate-caption/releases/tag/v1.0.0
