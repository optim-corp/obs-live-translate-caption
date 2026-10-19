#include "audio-convert.hpp"
#include "caption-output.hpp"
#include "caption-session.hpp"
#include "live-translate-session.hpp"
#include "translate-protocol.hpp"
#include "languages.hpp"
#include <atomic>
#include <cstring>
#include <media-io/audio-resampler.h>
#include <obs-module.h>
#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace {

// Distinct from the original plugin's "gemini_live_translate_filter" so the two
// never collide or get mixed up; filters saved by the original are not ours.
constexpr const char *kFilterId = "gemini_translate_caption_filter";
// Caption text box (spec 002 §4.1). `caption_max_lines` supersedes the legacy
// `caption_max_segments` key, which is still read for old scene collections.
constexpr int kMinCaptionLines = 1;
constexpr int kMaxCaptionLines = 6;
constexpr int kDefaultCaptionLines = 2;
constexpr int kMinCaptionWidth = 10;
constexpr int kMaxCaptionWidth = 120;
constexpr int kDefaultCaptionWidth = 60;
constexpr double kMinCaptionHoldSeconds = 1.0;
constexpr double kMaxCaptionHoldSeconds = 30.0;
// Auto-pause (spec 004 §4.1). 0 s disables the idle pause entirely; the dBFS
// threshold is user-facing because mic noise floors differ by an order of
// magnitude between a quiet USB mic and a laptop's built-in one.
constexpr int kMinIdleTimeoutSeconds = 0;
constexpr int kMaxIdleTimeoutSeconds = 1800;
constexpr int kDefaultIdleTimeoutSeconds = 300;
constexpr double kMinIdleThresholdDbfs = -90.0;
constexpr double kMaxIdleThresholdDbfs = -20.0;
constexpr double kDefaultIdleThresholdDbfs = -45.0;
// Translation models offered in the properties combo (the field stays editable).
constexpr const char *kTranslateModelChoices[] = {
    "gemini-3.1-flash-lite", "gemini-3.5-flash-lite", "gemini-3.5-flash",
    "gemini-flash-lite-latest", "gemini-flash-latest",
};
// Caption engines (spec 007 §4.5). "transcribe" is the original one-way
// pipeline; "live_two_way" runs two gemini-3.5-live-translate-preview sessions.
constexpr const char *kEngineTranscribe = "transcribe";
constexpr const char *kEngineTwoWay = "live_two_way";

struct FilterData {
    obs_source_t *context = nullptr;
    audio_resampler_t *resampler = nullptr;
    lt::Chunker chunker{3200};
    std::string engine = kEngineTranscribe;
    std::atomic<bool> two_way_selected{false}; // engine == kEngineTwoWay; read by the audio thread
    std::string two_way_lang_a = "ja";
    std::string two_way_lang_b = "en";
    std::string two_way_caption_a_source; // translations into lang_a
    std::string two_way_caption_b_source; // translations into lang_b
    std::string api_key;
    std::string target_lang = "en";
    std::string caption_text_source;
    std::string caption_source_text_source;
    bool caption_incremental_translation = true;
    int caption_max_lines = kDefaultCaptionLines;
    int caption_max_width = kDefaultCaptionWidth;
    double caption_hold_seconds = 4.0;
    std::string caption_custom_vocabulary;
    bool caption_quality_retry = true;
    std::string translate_model; // generateContent model id, "" = kTranslateModel
    int idle_timeout_seconds = kDefaultIdleTimeoutSeconds;
    double idle_threshold_dbfs = kDefaultIdleThresholdDbfs;
    bool only_while_output_active = false;
    bool caption_active = false; // true iff this filter drives the caption session
};

const char *filter_get_name(void *)
{
    return obs_module_text("FilterName");
}

// "foo, bar ,, baz" -> {"foo", "bar", "baz"}
std::vector<std::string> split_custom_vocabulary(const std::string &raw)
{
    static const char *kSpace = " \t\r\n";
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= raw.size()) {
        size_t comma = raw.find(',', start);
        size_t end = (comma == std::string::npos) ? raw.size() : comma;
        size_t b = raw.find_first_not_of(kSpace, start);
        if (b != std::string::npos && b < end) {
            size_t e = raw.find_last_not_of(kSpace, end - 1);
            out.push_back(raw.substr(b, e - b + 1));
        }
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return out;
}

lt::CaptionConfig make_caption_config(const FilterData *d)
{
    lt::CaptionConfig cfg;
    cfg.api_key = d->api_key;
    cfg.target_lang = d->target_lang;
    cfg.target_name = lt::target_language_name(d->target_lang);
    cfg.custom_vocabulary = split_custom_vocabulary(d->caption_custom_vocabulary);
    cfg.quality_retry = d->caption_quality_retry;
    cfg.translate_model = d->translate_model;
    cfg.incremental = d->caption_incremental_translation;
    cfg.max_lines = d->caption_max_lines;
    cfg.max_width = d->caption_max_width;
    cfg.hold_seconds = d->caption_hold_seconds;
    cfg.idle_timeout_seconds = d->idle_timeout_seconds;
    cfg.idle_threshold_dbfs = d->idle_threshold_dbfs;
    cfg.only_while_output_active = d->only_while_output_active;
    return cfg;
}

bool is_two_way(const FilterData *d)
{
    return d->two_way_selected.load();
}

lt::TwoWayConfig make_two_way_config(const FilterData *d)
{
    lt::TwoWayConfig cfg;
    cfg.api_key = d->api_key;
    cfg.lang_a = d->two_way_lang_a;
    cfg.lang_b = d->two_way_lang_b;
    cfg.max_lines = d->caption_max_lines;
    cfg.max_width = d->caption_max_width;
    cfg.hold_seconds = d->caption_hold_seconds;
    cfg.idle_timeout_seconds = d->idle_timeout_seconds;
    cfg.idle_threshold_dbfs = d->idle_threshold_dbfs;
    cfg.only_while_output_active = d->only_while_output_active;
    return cfg;
}

// Two-way source transcript: dropped when it collides with a caption source.
std::string two_way_transcript_name(const FilterData *d)
{
    const std::string &t = d->caption_source_text_source;
    if (t == d->two_way_caption_a_source || t == d->two_way_caption_b_source) return "";
    return t;
}

// Every text source the current engine writes to, so an update can blank the
// ones it no longer targets.
std::vector<std::string> written_text_sources(const FilterData *d)
{
    std::vector<std::string> names;
    if (is_two_way(d)) {
        names = {d->two_way_caption_a_source, d->two_way_caption_b_source,
                 two_way_transcript_name(d)};
    } else {
        names = {d->caption_text_source};
        if (d->caption_text_source.empty() ||
            d->caption_source_text_source != d->caption_text_source)
            names.push_back(d->caption_source_text_source);
    }
    names.erase(std::remove(names.begin(), names.end(), std::string()), names.end());
    return names;
}

lt::CaptionSession::TextSink text_sink(const std::string &name)
{
    if (name.empty()) return {};
    return [name](const std::string &text) { lt::caption_output_write(name, text); };
}

void install_two_way_sinks(const FilterData *d)
{
    lt::TwoWayTranslate::instance().set_sinks(text_sink(d->two_way_caption_a_source),
                                              text_sink(d->two_way_caption_b_source),
                                              text_sink(two_way_transcript_name(d)));
}

// (Re)installs the display sinks for the names this filter currently targets.
// Done on every update and on takeover, because the names may have changed or
// may still belong to a filter that is no longer primary.
void install_caption_sinks(const FilterData *d)
{
    std::string caption_name = d->caption_text_source;
    std::string source_name = d->caption_source_text_source;
    if (!caption_name.empty() && source_name == caption_name)
        source_name.clear();
    lt::CaptionSession::TextSink caption_sink =
        [caption_name](const std::string &text) {
            lt::caption_output_write(caption_name, text);
        };
    lt::CaptionSession::TextSink source_sink;
    if (!source_name.empty())
        source_sink = [source_name](const std::string &text) {
            lt::caption_output_write(source_name, text);
        };
    lt::CaptionSession::instance().set_sinks(std::move(caption_sink),
                                             std::move(source_sink));
}

// stop() resets the status to Idle, which would hide an API key error from the
// properties panel. A settings change that is not about the key must not wipe
// that message, so leave an auth-stopped session alone (spec criterion 7).
void stop_caption_session_for_settings()
{
    auto &session = lt::CaptionSession::instance();
    if (!session.is_running() && session.status() == lt::ConnStatus::AuthError)
        return;
    session.stop();
}

void stop_two_way_for_settings()
{
    auto &two_way = lt::TwoWayTranslate::instance();
    if (!two_way.is_running() && two_way.has_auth_error()) return;
    two_way.stop();
}

// Starts or reconfigures the selected engine and stops the other one.
void run_selected_engine(const FilterData *d)
{
    // The idle engine's sinks are dropped so it can never blank a source the
    // selected engine now writes to; it is stopped only if it still runs.
    if (is_two_way(d)) {
        auto &other = lt::CaptionSession::instance();
        if (other.is_running()) other.stop();
        other.set_sinks({}, {});
        install_two_way_sinks(d);
        lt::TwoWayTranslate::instance().configure(make_two_way_config(d));
    } else {
        auto &other = lt::TwoWayTranslate::instance();
        if (other.is_running()) other.stop();
        other.set_sinks({}, {}, {});
        install_caption_sinks(d);
        lt::CaptionSession::instance().configure(make_caption_config(d));
    }
}

struct PrimaryScan {
    obs_source_t *first; // first Gemini filter on the parent, in filter order
};

void primary_scan_cb(obs_source_t * /*parent*/, obs_source_t *child, void *param)
{
    auto *s = static_cast<PrimaryScan *>(param);
    if (!s->first && std::strcmp(obs_source_get_id(child), kFilterId) == 0)
        s->first = child;
}

// Per-source first-wins: a filter is "primary" iff it is the first Gemini Live
// Translate filter on its parent source. A second copy on the SAME source is
// not primary (it gets disabled + warned). This queries live OBS state on every
// call, so there is no persistent owner that can go stale. Note: this does NOT
// coordinate across different sources — the shared session has a single stream,
// so putting the filter on two different sources is unsupported (see README).
bool is_primary_filter(FilterData *d)
{
    obs_source_t *parent = obs_filter_get_parent(d->context);
    if (!parent) return true; // not attached yet; don't show a false warning
    PrimaryScan scan{nullptr};
    obs_source_enum_filters(parent, primary_scan_cb, &scan);
    return scan.first == nullptr || scan.first == d->context;
}

void create_resampler(FilterData *d)
{
    if (d->resampler) {
        audio_resampler_destroy(d->resampler);
        d->resampler = nullptr;
    }
    audio_t *audio = obs_get_audio();
    struct resample_info src = {};
    src.samples_per_sec = audio_output_get_sample_rate(audio);
    src.format = AUDIO_FORMAT_FLOAT_PLANAR;
    src.speakers = audio_output_get_channels(audio) == 1 ? SPEAKERS_MONO
                                                          : SPEAKERS_STEREO;
    struct resample_info dst = {};
    dst.samples_per_sec = 16000;
    dst.format = AUDIO_FORMAT_16BIT;
    dst.speakers = SPEAKERS_MONO;
    d->resampler = audio_resampler_create(&dst, &src);
}

void filter_update(void *data, obs_data_t *settings)
{
    auto *d = static_cast<FilterData *>(data);
    const std::vector<std::string> old_targets = written_text_sources(d);
    const bool was_two_way = is_two_way(d);

    d->api_key = obs_data_get_string(settings, "api_key");
    d->engine = obs_data_get_string(settings, "engine");
    if (d->engine != kEngineTwoWay) d->engine = kEngineTranscribe;
    d->two_way_selected = d->engine == kEngineTwoWay;
    d->target_lang = obs_data_get_string(settings, "target_lang");
    d->two_way_lang_a = obs_data_get_string(settings, "two_way_lang_a");
    d->two_way_lang_b = obs_data_get_string(settings, "two_way_lang_b");
    d->two_way_caption_a_source = obs_data_get_string(settings, "two_way_caption_a_text_source");
    d->two_way_caption_b_source = obs_data_get_string(settings, "two_way_caption_b_text_source");
    d->caption_text_source = obs_data_get_string(settings, "caption_text_source");
    d->caption_source_text_source =
        obs_data_get_string(settings, "caption_source_text_source");

    // A name we no longer target keeps whatever text was written to it last;
    // blank it so a stale caption does not linger on screen. Never clear a
    // source that one of the new sinks writes to: in particular, an old source-
    // transcript sink may become the translated-caption sink in this update.
    const std::vector<std::string> new_targets = written_text_sources(d);
    for (const auto &name : old_targets)
        if (std::find(new_targets.begin(), new_targets.end(), name) == new_targets.end())
            lt::caption_output_write(name, "");
    // Scene collections written before spec 002 only carry the legacy segment
    // count; read it as the line count when the new key was never stored
    // (spec 002 §4.1, criterion 12). filter_defaults deliberately registers no
    // default for the legacy key, so obs_data_has_user_value can tell a stored
    // value from an absent one.
    long long lines = kDefaultCaptionLines;
    if (obs_data_has_user_value(settings, "caption_max_lines"))
        lines = obs_data_get_int(settings, "caption_max_lines");
    else if (obs_data_has_user_value(settings, "caption_max_segments"))
        lines = obs_data_get_int(settings, "caption_max_segments");
    d->caption_max_lines = static_cast<int>(
        std::clamp<long long>(lines, kMinCaptionLines, kMaxCaptionLines));
    d->caption_max_width = static_cast<int>(std::clamp<long long>(
        obs_data_get_int(settings, "caption_max_chars_per_line"),
        kMinCaptionWidth, kMaxCaptionWidth));
    d->caption_hold_seconds =
        std::clamp(obs_data_get_double(settings, "caption_hold_seconds"),
                   kMinCaptionHoldSeconds, kMaxCaptionHoldSeconds);
    d->translate_model = obs_data_get_string(settings, "translate_model");
    // Clamped here rather than trusted: SetSourceFilterSettings can carry any
    // value, and the sliders' bounds only constrain the UI (criterion 12).
    d->idle_timeout_seconds = static_cast<int>(std::clamp<long long>(
        obs_data_get_int(settings, "idle_timeout_seconds"),
        kMinIdleTimeoutSeconds, kMaxIdleTimeoutSeconds));
    d->idle_threshold_dbfs =
        std::clamp(obs_data_get_double(settings, "idle_threshold_dbfs"),
                   kMinIdleThresholdDbfs, kMaxIdleThresholdDbfs);
    d->only_while_output_active =
        obs_data_get_bool(settings, "only_while_output_active");
    d->caption_incremental_translation =
        obs_data_get_bool(settings, "caption_incremental_translation");
    d->caption_custom_vocabulary =
        obs_data_get_string(settings, "caption_custom_vocabulary");
    d->caption_quality_retry = obs_data_get_bool(settings, "caption_quality_retry");

    bool run = !d->api_key.empty() && is_primary_filter(d);
    if (run) {
        d->caption_active = true;
        // Re-install every update: the target names may have changed.
        run_selected_engine(d);
    } else if (d->caption_active) {
        // Key cleared, or another filter is now first. A remaining primary
        // filter restarts the session from its own audio callback.
        d->caption_active = false;
        if (was_two_way || is_two_way(d)) stop_two_way_for_settings();
        if (!was_two_way || !is_two_way(d)) stop_caption_session_for_settings();
    }
}

void *filter_create(obs_data_t *settings, obs_source_t *source)
{
    auto *d = new FilterData();
    d->context = source;
    create_resampler(d);
    filter_update(d, settings);
    return d;
}

void filter_destroy(void *data)
{
    auto *d = static_cast<FilterData *>(data);
    // If we were feeding the session, stop it. A remaining same-source filter
    // (if any) becomes primary and restarts it from its next audio callback.
    if (d->caption_active) {
        if (is_two_way(d))
            lt::TwoWayTranslate::instance().stop();
        else
            lt::CaptionSession::instance().stop();
        // stop() already blanks the sources through our sinks; clear the names
        // directly as well, in case the sinks were replaced meanwhile. Only the
        // driving filter does this, so a passive duplicate cannot wipe the
        // captions of the filter that is actually running.
        for (const auto &name : written_text_sources(d))
            lt::caption_output_write(name, "");
    }
    if (d->resampler) audio_resampler_destroy(d->resampler);
    delete d;
}

struct obs_audio_data *filter_audio(void *data, struct obs_audio_data *audio)
{
    auto *d = static_cast<FilterData *>(data);
    if (!d->resampler || d->api_key.empty()) return audio;

    auto &captions = lt::CaptionSession::instance();
    auto &two_way = lt::TwoWayTranslate::instance();
    const bool use_two_way = is_two_way(d);
    // Only the primary (first) Gemini filter on this source feeds the session;
    // a duplicate on the same source stays a passive pass-through. Recomputed
    // live each callback, so when the primary is removed this one takes over.
    bool primary = is_primary_filter(d);
    bool became_primary = primary && !d->caption_active;
    if (primary != d->caption_active) {
        d->caption_active = primary;
        // Primary status changed: refresh the (possibly open) properties panel
        // so a stale "disabled" warning clears. Safe from the audio thread —
        // OBS marshals the refresh to the UI thread.
        obs_source_update_properties(d->context);
    }
    if (!primary) return audio;

    // (Re)configure when we take over, and also if the session was cleanly
    // stopped while we are primary — e.g. the previous primary's teardown
    // raced our takeover and stopped the session right after we claimed it.
    // Skip when it stopped on an auth error, so a bad key doesn't spin-restart.
    // configure() only spawns the worker (or reaps an already-exited one); it
    // never joins a live connection, so it won't stall the audio callback.
    bool needs_start = use_two_way
        ? !two_way.is_running() && !two_way.has_auth_error()
        : !captions.is_running() && captions.status() != lt::ConnStatus::AuthError;
    if (became_primary || needs_start) {
        // The sinks may still be the previous primary's; claim them.
        run_selected_engine(d);
    }

    uint8_t *out[MAX_AV_PLANES] = {};
    uint32_t out_frames = 0;
    uint64_t ts_offset = 0;
    bool ok = audio_resampler_resample(
        d->resampler, out, &out_frames, &ts_offset,
        (const uint8_t *const *)audio->data, audio->frames);
    if (ok && out_frames > 0) {
        // Send the audio continuously, including silence, the way the official
        // Live API client streams the mic. Gating out silence made our input
        // stream discontinuous, which disrupts the model's real-time VAD/turn
        // handling and delays translation output until the next speech resumes.
        auto chunks = d->chunker.push(out[0], out_frames * 2);
        for (auto &c : chunks) {
            if (use_two_way)
                two_way.push_input_pcm(c.data(), c.size());
            else
                captions.push_input_pcm(c.data(), c.size());
        }
    }
    return audio;
}

struct TextSourceLists {
    std::vector<obs_property_t *> lists;
};

bool add_text_source_cb(void *param, obs_source_t *source)
{
    const char *id = obs_source_get_unversioned_id(source);
    if (!id) return true;
    // Windows uses text_gdiplus, macOS/Linux text_ft2_source. The versioned id
    // (text_gdiplus_v3) is not usable for this check.
    if (std::strcmp(id, "text_gdiplus") != 0 &&
        std::strcmp(id, "text_ft2_source") != 0)
        return true;
    const char *name = obs_source_get_name(source);
    if (!name || !*name) return true;
    auto *lists = static_cast<TextSourceLists *>(param);
    for (auto *list : lists->lists)
        obs_property_list_add_string(list, name, name);
    return true;
}

std::string filter_status_text(FilterData *d)
{
    if (d && !is_primary_filter(d))
        return obs_module_text("Status.DuplicateFilter");
    if (d && is_two_way(d)) {
        if (d->two_way_lang_a == d->two_way_lang_b)
            return obs_module_text("Status.TwoWaySameLanguage");
        if (!d->two_way_caption_a_source.empty() &&
            d->two_way_caption_a_source == d->two_way_caption_b_source)
            return obs_module_text("Status.TwoWaySameCaptionSource");
        if (!d->caption_source_text_source.empty() && two_way_transcript_name(d).empty())
            return obs_module_text("Status.SameTextSource");
    } else if (d && !d->caption_text_source.empty() &&
               d->caption_source_text_source == d->caption_text_source) {
        return obs_module_text("Status.SameTextSource");
    }
    std::string missing = lt::caption_output_missing_source();
    if (!missing.empty())
        return "Caption text source \"" + missing + "\" not found";
    if (d && is_two_way(d)) {
        if (d->two_way_caption_a_source.empty() && d->two_way_caption_b_source.empty())
            return obs_module_text("Status.NoCaptionSource");
        return lt::TwoWayTranslate::instance().status_text();
    }
    if (d && d->caption_text_source.empty())
        return obs_module_text("Status.NoCaptionSource");
    return lt::CaptionSession::instance().status_text();
}

// Shows only the settings of the selected engine.
bool engine_modified(obs_properties_t *props, obs_property_t *, obs_data_t *settings)
{
    const bool two_way = std::strcmp(obs_data_get_string(settings, "engine"), kEngineTwoWay) == 0;
    for (const char *name : {"target_lang", "caption_text_source", "caption_text_source_info",
                             "caption_custom_vocabulary", "caption_quality_retry",
                             "caption_incremental_translation",
                             "caption_incremental_translation_info", "translate_model"})
        obs_property_set_visible(obs_properties_get(props, name), !two_way);
    for (const char *name : {"two_way_lang_a", "two_way_lang_b", "two_way_caption_a_text_source",
                             "two_way_caption_b_text_source", "two_way_info"})
        obs_property_set_visible(obs_properties_get(props, name), two_way);
    return true;
}

obs_property_t *add_language_list(obs_properties_t *props, const char *name, const char *label)
{
    obs_property_t *list = obs_properties_add_list(props, name, label, OBS_COMBO_TYPE_LIST,
                                                   OBS_COMBO_FORMAT_STRING);
    for (int i = 0; i < lt::kLanguagesCount; ++i)
        obs_property_list_add_string(list, lt::kLanguages[i].name, lt::kLanguages[i].code);
    return list;
}

obs_properties_t *filter_properties(void *data)
{
    obs_properties_t *props = obs_properties_create();

    obs_property_t *engine = obs_properties_add_list(
        props, "engine", obs_module_text("Engine"), OBS_COMBO_TYPE_LIST,
        OBS_COMBO_FORMAT_STRING);
    obs_property_list_add_string(engine, obs_module_text("Engine.Transcribe"), kEngineTranscribe);
    obs_property_list_add_string(engine, obs_module_text("Engine.TwoWay"), kEngineTwoWay);
    obs_property_set_modified_callback(engine, engine_modified);

    add_language_list(props, "target_lang", obs_module_text("TargetLanguage"));
    add_language_list(props, "two_way_lang_a", obs_module_text("TwoWayLanguageA"));
    add_language_list(props, "two_way_lang_b", obs_module_text("TwoWayLanguageB"));

    // Editable combos: the text source may not exist yet, so its name can be
    // typed in before the source is created (spec §4.1).
    TextSourceLists lists;
    for (const auto &[name, label] : {
             std::pair{"caption_text_source", "CaptionTextSource"},
             std::pair{"two_way_caption_a_text_source", "TwoWayCaptionATextSource"},
             std::pair{"two_way_caption_b_text_source", "TwoWayCaptionBTextSource"},
             std::pair{"caption_source_text_source", "SourceTranscriptTextSource"}}) {
        obs_property_t *list = obs_properties_add_list(
            props, name, obs_module_text(label), OBS_COMBO_TYPE_EDITABLE,
            OBS_COMBO_FORMAT_STRING);
        obs_property_list_add_string(list, obs_module_text("None"), "");
        lists.lists.push_back(list);
    }
    obs_enum_sources(add_text_source_cb, &lists);
    obs_properties_add_text(props, "two_way_info", obs_module_text("TwoWayInfo"),
                            OBS_TEXT_INFO);
    obs_properties_add_text(
        props, "caption_text_source_info",
        obs_module_text("CaptionTextSourceInfo"), OBS_TEXT_INFO);

    obs_properties_add_int_slider(
        props, "caption_max_lines", obs_module_text("CaptionLines"),
        kMinCaptionLines, kMaxCaptionLines, 1);
    obs_properties_add_int_slider(
        props, "caption_max_chars_per_line",
        obs_module_text("MaxCharsPerLine"),
        kMinCaptionWidth, kMaxCaptionWidth, 1);
    obs_properties_add_float_slider(
        props, "caption_hold_seconds",
        obs_module_text("CaptionHoldSeconds"), kMinCaptionHoldSeconds,
        kMaxCaptionHoldSeconds, 0.5);
    obs_properties_add_text(
        props, "caption_custom_vocabulary",
        obs_module_text("CustomVocabulary"), OBS_TEXT_DEFAULT);
    obs_properties_add_bool(
        props, "caption_quality_retry", obs_module_text("QualityRetry"));
    obs_properties_add_bool(
        props, "caption_incremental_translation",
        obs_module_text("IncrementalTranslation"));
    obs_properties_add_text(
        props, "caption_incremental_translation_info",
        obs_module_text("IncrementalTranslationInfo"), OBS_TEXT_INFO);

    // Editable so a model id that is not listed yet can be typed in. Ids come
    // from the Gemini model docs; availability depends on the account.
    obs_property_t *model_list = obs_properties_add_list(
        props, "translate_model", obs_module_text("TranslationModel"),
        OBS_COMBO_TYPE_EDITABLE, OBS_COMBO_FORMAT_STRING);
    for (const char *id : kTranslateModelChoices)
        obs_property_list_add_string(model_list, id, id);

    // Auto-pause (spec 004 §4.1). These apply live; they never reconnect.
    obs_properties_add_int_slider(
        props, "idle_timeout_seconds",
        obs_module_text("IdleTimeout"),
        kMinIdleTimeoutSeconds, kMaxIdleTimeoutSeconds, 10);
    obs_properties_add_float_slider(
        props, "idle_threshold_dbfs", obs_module_text("IdleThreshold"),
        kMinIdleThresholdDbfs, kMaxIdleThresholdDbfs, 1.0);
    obs_properties_add_bool(
        props, "only_while_output_active",
        obs_module_text("OnlyWhileOutputActive"));

    obs_properties_add_text(props, "api_key", obs_module_text("ApiKey"),
                            OBS_TEXT_PASSWORD);
    obs_properties_add_text(
        props, "warn",
        obs_module_text("ApiKeyWarning"),
        OBS_TEXT_INFO);
    auto *d = static_cast<FilterData *>(data);
    std::string status = filter_status_text(d);
    obs_properties_add_text(props, "status", status.c_str(), OBS_TEXT_INFO);
    return props;
}

void filter_defaults(obs_data_t *settings)
{
    obs_data_set_default_string(settings, "engine", kEngineTranscribe);
    obs_data_set_default_string(settings, "target_lang", "en");
    obs_data_set_default_string(settings, "two_way_lang_a", "ja");
    obs_data_set_default_string(settings, "two_way_lang_b", "en");
    obs_data_set_default_string(settings, "two_way_caption_a_text_source", "");
    obs_data_set_default_string(settings, "two_way_caption_b_text_source", "");
    obs_data_set_default_string(settings, "caption_text_source", "");
    obs_data_set_default_string(settings, "caption_source_text_source", "");
    obs_data_set_default_int(settings, "caption_max_lines", kDefaultCaptionLines);
    obs_data_set_default_int(settings, "caption_max_chars_per_line",
                             kDefaultCaptionWidth);
    // No default for the legacy "caption_max_segments": filter_update relies on
    // obs_data_has_user_value() to detect a stored legacy value.
    obs_data_set_default_double(settings, "caption_hold_seconds", 4.0);
    obs_data_set_default_string(settings, "caption_custom_vocabulary", "");
    obs_data_set_default_bool(settings, "caption_quality_retry", true);
    obs_data_set_default_bool(settings, "caption_incremental_translation", true);
    obs_data_set_default_string(settings, "translate_model", lt::kTranslateModel);
    // Absent from scene collections written before spec 004; the defaults keep
    // those behaving as before except for the 5-minute idle pause (criterion 13).
    obs_data_set_default_int(settings, "idle_timeout_seconds",
                             kDefaultIdleTimeoutSeconds);
    obs_data_set_default_double(settings, "idle_threshold_dbfs",
                                kDefaultIdleThresholdDbfs);
    obs_data_set_default_bool(settings, "only_while_output_active", false);
}

void filter_get_status(void *data, obs_data_t *settings)
{
    obs_data_set_string(
        settings, "status",
        filter_status_text(static_cast<FilterData *>(data)).c_str());
}

} // namespace

struct obs_source_info live_translate_filter_info = [] {
    struct obs_source_info info = {};
    info.id = kFilterId;
    info.type = OBS_SOURCE_TYPE_FILTER;
    info.output_flags = OBS_SOURCE_AUDIO;
    info.get_name = filter_get_name;
    info.create = filter_create;
    info.destroy = filter_destroy;
    info.update = filter_update;
    info.filter_audio = filter_audio;
    info.get_properties = filter_properties;
    info.get_defaults = filter_defaults;
    return info;
}();
