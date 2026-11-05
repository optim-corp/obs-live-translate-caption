#include "live-translate-session.hpp"
#include "audio-convert.hpp"
#include "backoff.hpp"
#include "caption-protocol.hpp"
#include "live-translate-protocol.hpp"
#include <ixwebsocket/IXWebSocket.h>
#include <obs.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <optional>
#include <utility>
#include <vector>

// Two-way caption engine (docs/specs/007-two-way-live-translate.md §4.3, §4.4).
// Per session: WebSocket -> transcription deltas -> LiveCaptionBuffer -> sinks.
// The translated audio the model streams back is parsed past and dropped.

namespace lt {

namespace {

uint64_t now_ms()
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

uint64_t elapsed_ms(uint64_t since, uint64_t now)
{
    return now >= since ? now - since : 0;
}

// Close codes the Live API uses to reject a setup (bad key, unknown model,
// invalid translationConfig). Retrying cannot fix those, so they stop the session.
bool is_fatal_close(uint16_t code)
{
    return code == 1007 || code == 1008;
}

std::string bounded_reason(std::string reason)
{
    constexpr size_t kMax = 400; // long enough for a full setup-rejection message
    if (reason.size() > kMax) {
        size_t cut = kMax;
        while (cut > 0 && (static_cast<unsigned char>(reason[cut]) & 0xc0) == 0x80) --cut;
        reason.resize(cut);
        reason += "...";
    }
    return reason;
}

} // namespace

// ---------------------------------------------------------------------------
// LiveTranslateSession

LiveTranslateSession::LiveTranslateSession(std::string label, PauseGate gate,
                                           std::function<void()> on_tick)
    : label_(std::move(label)), gate_(std::move(gate)), on_tick_(std::move(on_tick))
{
}

LiveTranslateSession::~LiveTranslateSession() { stop(); }

void LiveTranslateSession::set_status(ConnStatus s, const std::string &detail)
{
    std::lock_guard<std::mutex> lk(status_mtx_);
    status_ = s;
    status_detail_ = detail;
}

ConnStatus LiveTranslateSession::status()
{
    std::lock_guard<std::mutex> lk(status_mtx_);
    return status_;
}

std::string LiveTranslateSession::status_text()
{
    std::lock_guard<std::mutex> lk(status_mtx_);
    switch (status_) {
    case ConnStatus::Idle:
        return "Idle";
    case ConnStatus::Connecting:
        return "Connecting...";
    case ConnStatus::Connected:
        return "Connected";
    case ConnStatus::Reconnecting:
        return status_detail_.empty() ? "Reconnecting..." : "Reconnecting... (" + status_detail_ + ")";
    case ConnStatus::AuthError:
        return "Error: " + status_detail_;
    case ConnStatus::Paused:
        return "Paused (" + status_detail_ + ")";
    }
    return "Unknown";
}

void LiveTranslateSession::set_sinks(TextSink caption, TextSink transcript)
{
    {
        std::lock_guard<std::mutex> lk(mtx_);
        caption_sink_ = std::move(caption);
        transcript_sink_ = std::move(transcript);
        // Force the next publish to write the new targets.
        shown_caption_ = "\x01";
        shown_transcript_ = "\x01";
    }
    publish();
}

void LiveTranslateSession::configure(const Config &cfg)
{
    if (cfg.api_key.empty()) { stop(); return; }
    std::lock_guard<std::mutex> lifecycle(lifecycle_mtx_);
    bool start;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        start = !running_.load();
        const bool target_changed = cfg_.target_lang != cfg.target_lang;
        if (start || cfg_.api_key != cfg.api_key || target_changed) {
            proven_ = false; // a new key/target must be accepted once before retries are trusted
            if (!start) config_changed_ = true;
        }
        if (target_changed) {
            caption_.clear();
            transcript_.clear();
        }
        cfg_ = cfg;
    }
    if (start) {
        // Reap a worker that stopped itself (setup rejected).
        if (thread_.joinable()) thread_.join();
        input_.clear();
        config_changed_ = false;
        running_ = true;
        thread_ = std::thread([this] { run(); });
    }
    publish(); // box settings apply live
}

void LiveTranslateSession::stop()
{
    std::lock_guard<std::mutex> lifecycle(lifecycle_mtx_);
    running_ = false;
    if (thread_.joinable()) thread_.join();
    input_.clear();
    {
        std::lock_guard<std::mutex> lk(mtx_);
        caption_.clear();
        transcript_.clear();
    }
    publish();
    announced_pause_.clear();
    set_status(ConnStatus::Idle);
}

void LiveTranslateSession::push_input_pcm(const uint8_t *data, size_t len)
{
    // Buffered while paused too; the resume trims it to a short pre-roll.
    input_.write(data, len);
}

void LiveTranslateSession::publish()
{
    std::lock_guard<std::mutex> publication(publish_mtx_);
    std::optional<std::string> caption, transcript;
    TextSink caption_sink, transcript_sink;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        std::string c = caption_.render(cfg_.max_width, cfg_.max_lines);
        if (c != shown_caption_) {
            shown_caption_ = c;
            caption = std::move(c);
            caption_sink = caption_sink_;
        }
        std::string t = transcript_.render(cfg_.max_width, cfg_.max_lines);
        if (t != shown_transcript_) {
            shown_transcript_ = t;
            transcript = std::move(t);
            transcript_sink = transcript_sink_;
        }
    }
    if (caption && caption_sink) caption_sink(*caption);
    if (transcript && transcript_sink) transcript_sink(*transcript);
}

void LiveTranslateSession::tick()
{
    {
        std::lock_guard<std::mutex> lk(mtx_);
        const uint64_t now = now_ms();
        caption_.expire(now, cfg_.hold_ms);
        transcript_.expire(now, cfg_.hold_ms);
    }
    publish();
    if (on_tick_) on_tick_();

    // One line a minute while connected, so a silent stretch in an event can be
    // told apart later: no speech recognized (in=0) vs. no translation (out=0).
    const uint64_t now = now_ms();
    if (elapsed_ms(last_stats_ms_, now) >= kStatsMs) {
        last_stats_ms_ = now;
        if (connected_since_.load()) {
            const uint64_t last_out = last_output_ms_.load();
            blog(LOG_INFO, "[live-translate] two-way %s stats: messages=%llu input_deltas=%llu "
                 "output_deltas=%llu last_output_ms_ago=%lld", label_.c_str(),
                 static_cast<unsigned long long>(messages_.exchange(0)),
                 static_cast<unsigned long long>(input_deltas_.exchange(0)),
                 static_cast<unsigned long long>(output_deltas_.exchange(0)),
                 last_out ? static_cast<long long>(elapsed_ms(last_out, now)) : -1LL);
        }
    }
}

void LiveTranslateSession::announce_pause(const LivePauseState &state)
{
    const std::string detail = state.waiting_for_sound ? "waiting for sound"
                                                       : pause_reason_name(state.reason);
    if (detail != announced_pause_) {
        blog(LOG_INFO, "[live-translate] two-way %s paused: %s", label_.c_str(), detail.c_str());
        announced_pause_ = detail;
    }
    set_status(ConnStatus::Paused, detail);
}

bool LiveTranslateSession::wait_while_paused(LivePauseState state)
{
    announce_pause(state);
    while (running_) {
        state = gate_();
        if (state.reason == PauseReason::None) break;
        announce_pause(state);
        tick(); // captions still clear on time while paused
        std::this_thread::sleep_for(std::chrono::milliseconds(kTickMs));
    }
    if (!running_) return false;
    blog(LOG_INFO, "[live-translate] two-way %s resumed", label_.c_str());
    announced_pause_.clear();
    input_.keep_last(kResumePrerollBytes);
    return true;
}

void LiveTranslateSession::run()
{
    Backoff backoff(1000, 30000);
    announced_pause_.clear();

    while (running_) {
        std::string key, target;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            key = cfg_.api_key;
            target = cfg_.target_lang;
            config_changed_ = false;
        }

        LivePauseState gate = gate_();
        if (gate.reason != PauseReason::None) {
            backoff.reset();
            if (!wait_while_paused(gate)) break;
            continue; // re-read the config, re-check the gate, then connect
        }

        ix::WebSocket ws;
        ws.setUrl("wss://generativelanguage.googleapis.com/ws/"
                  "google.ai.generativelanguage.v1beta.GenerativeService."
                  "BidiGenerateContent?key=" + key);
        ws.disableAutomaticReconnection();
        // Base64 PCM barely compresses; deflate only adds CPU and a framing layer
        // that is a suspect for the server's sporadic "Invalid JSON payload" (1007).
        ws.disablePerMessageDeflate();

        std::atomic<bool> open{false};
        std::atomic<bool> disconnected{false};
        std::atomic<bool> fatal{false};
        std::atomic<bool> setup_ok{false}; // the server accepted this connection's setup
        std::atomic<bool> rotate{false};
        rotate_requested_ = false;
        std::atomic<uint64_t> connected_at{0};
        std::mutex reason_mtx;
        std::string close_reason;

        ws.setOnMessageCallback([&](const ix::WebSocketMessagePtr &msg) {
            if (msg->type == ix::WebSocketMessageType::Open) {
                set_status(ConnStatus::Connected);
                connected_at = now_ms();
                connected_since_ = connected_at.load();
                ws.send(build_live_translate_setup_message(target, false));
                open = true;
                blog(LOG_INFO, "[live-translate] two-way %s websocket opened; setup sent (target=%s)",
                     label_.c_str(), target.c_str());
            } else if (msg->type == ix::WebSocketMessageType::Message) {
                const auto m = parse_live_translate_server_message(msg->str);
                if (m.error) {
                    std::lock_guard<std::mutex> lk(reason_mtx);
                    close_reason = bounded_reason(m.error_message.empty() ? "request rejected"
                                                                          : m.error_message);
                    fatal = true;
                    return;
                }
                ++messages_;
                if (m.setup_complete) {
                    setup_ok = true;
                    blog(LOG_INFO, "[live-translate] two-way %s setup complete", label_.c_str());
                }
                if (m.go_away) rotate = true;
                if (m.output_text.empty() && m.input_text.empty() && !m.turn_complete) return;
                setup_ok = true;
                {
                    std::lock_guard<std::mutex> lk(mtx_);
                    const uint64_t now = now_ms();
                    if (!m.output_text.empty()) {
                        last_output_ms_ = now;
                        ++output_deltas_;
                    }
                    if (!m.input_text.empty()) {
                        last_input_ms_ = now;
                        ++input_deltas_;
                    }
                    if (!m.output_text.empty()) caption_.append(m.output_text, now);
                    if (!m.input_text.empty()) transcript_.append(m.input_text, now);
                    if (m.turn_complete) {
                        caption_.end_turn();
                        transcript_.end_turn();
                    }
                }
                publish();
            } else if (msg->type == ix::WebSocketMessageType::Close ||
                       msg->type == ix::WebSocketMessageType::Error) {
                std::string reason;
                if (msg->type == ix::WebSocketMessageType::Close) {
                    reason = "closed (" + std::to_string(msg->closeInfo.code) + ")";
                    if (!msg->closeInfo.reason.empty()) reason += ": " + msg->closeInfo.reason;
                    if (is_fatal_close(msg->closeInfo.code)) fatal = true;
                } else {
                    reason = "transport error";
                }
                {
                    std::lock_guard<std::mutex> lk(reason_mtx);
                    if (close_reason.empty()) close_reason = bounded_reason(reason);
                }
                open = false;
                disconnected = true;
            }
        });

        set_status(ConnStatus::Connecting);
        ws.start();

        std::vector<uint8_t> chunk(kChunkBytes);
        uint64_t last_tick = 0;
        LivePauseState pause;

        while (running_ && !fatal && !config_changed_ && !disconnected && !rotate) {
            if (rotate_requested_.exchange(false)) {
                rotate = true; // stall watchdog (TwoWayTranslate::watchdog)
                break;
            }
            const size_t n = open ? input_.read(chunk.data(), chunk.size()) : 0;
            if (n == chunk.size())
                ws.send(build_realtime_input_message(chunk.data(), chunk.size()));
            else
                std::this_thread::sleep_for(std::chrono::milliseconds(10));

            const uint64_t now = now_ms();
            if (elapsed_ms(last_tick, now) >= kTickMs) {
                last_tick = now;
                tick();
                pause = gate_();
                if (pause.reason != PauseReason::None) break;
            }
            const uint64_t since = connected_at.load();
            if (since != 0 && elapsed_ms(since, now) >= kSessionMaxMs) {
                blog(LOG_INFO, "[live-translate] two-way %s reconnect: session age", label_.c_str());
                rotate = true;
            }
        }

        ws.stop();
        connected_since_ = 0;
        // Only a connection the server accepted proves the settings work; a
        // socket that opens and is rejected at setup (quota) keeps backing off.
        if (setup_ok) {
            backoff.reset();
            proven_ = true;
        }

        std::string reason;
        {
            std::lock_guard<std::mutex> lk(reason_mtx);
            reason = close_reason.empty() ? "closed" : close_reason;
        }
        if (fatal && !proven_) {
            // Rejected before this run ever worked (bad key, unknown model):
            // retrying cannot help. No URL is logged: it carries the key.
            blog(LOG_WARNING, "[live-translate] two-way %s stopped: %s", label_.c_str(), reason.c_str());
            set_status(ConnStatus::AuthError, reason);
            running_ = false;
            break;
        }
        if (fatal) {
            // Worked before, so this is mid-event trouble (quota, a corrupted
            // frame): keep captions alive by reconnecting with backoff.
            blog(LOG_WARNING, "[live-translate] two-way %s server error, will reconnect: %s",
                 label_.c_str(), reason.c_str());
        }
        if (pause.reason != PauseReason::None || rotate || config_changed_) {
            if (rotate) blog(LOG_INFO, "[live-translate] two-way %s rotating session", label_.c_str());
            continue; // the gate at the top parks us while paused
        }
        if (!running_) break;

        blog(LOG_INFO, "[live-translate] two-way %s reconnect: %s", label_.c_str(), reason.c_str());
        set_status(ConnStatus::Reconnecting, reason);
        const uint32_t wait = backoff.next_ms();
        for (uint32_t waited = 0; waited < wait && running_ && !config_changed_ &&
                                  gate_().reason == PauseReason::None;
             waited += 50) {
            if (waited % kTickMs == 0) tick();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }

    {
        std::lock_guard<std::mutex> lk(mtx_);
        caption_.clear();
        transcript_.clear();
    }
    publish();
    if (status() != ConnStatus::AuthError) set_status(ConnStatus::Idle);
}

// ---------------------------------------------------------------------------
// TwoWayTranslate

TwoWayTranslate &TwoWayTranslate::instance()
{
    static TwoWayTranslate s;
    return s;
}

TwoWayTranslate::TwoWayTranslate()
    : to_a_("to-a", [this] { return pause_state(); }, [this] { watchdog(); }),
      to_b_("to-b", [this] { return pause_state(); }, [this] { watchdog(); })
{
    const TwoWayConfig defaults;
    idle_threshold_rms_.store(dbfs_to_rms(defaults.idle_threshold_dbfs));
    idle_detector_.configure(static_cast<uint64_t>(defaults.idle_timeout_seconds) * 1000);
}

TwoWayTranslate::~TwoWayTranslate() { stop(); }

void TwoWayTranslate::set_sinks(TextSink caption_a, TextSink caption_b, TextSink transcript)
{
    to_a_.set_sinks(std::move(caption_a), std::move(transcript));
    to_b_.set_sinks(std::move(caption_b), nullptr);
}

void TwoWayTranslate::configure(const TwoWayConfig &cfg)
{
    if (cfg.api_key.empty()) { stop(); return; }
    const bool start = !is_running();
    {
        std::lock_guard<std::mutex> lk(cfg_mtx_);
        lang_a_ = cfg.lang_a;
        lang_b_ = cfg.lang_b;
    }
    idle_threshold_rms_.store(dbfs_to_rms(cfg.idle_threshold_dbfs));
    only_while_output_.store(cfg.only_while_output_active);
    {
        std::lock_guard<std::mutex> lk(idle_mtx_);
        idle_detector_.configure(static_cast<uint64_t>(std::max(0, cfg.idle_timeout_seconds)) * 1000);
        // A fresh start waits for the first words instead of billing silence.
        if (start) idle_detector_.start_idle(now_ms());
    }

    const double hold = std::isfinite(cfg.hold_seconds) ? cfg.hold_seconds : 4.0;
    LiveTranslateSession::Config base;
    base.api_key = cfg.api_key;
    base.max_lines = cfg.max_lines;
    base.max_width = cfg.max_width;
    base.hold_ms = static_cast<uint64_t>(std::clamp(hold * 1000.0, 1000.0, 30000.0));

    auto a = base;
    a.target_lang = cfg.lang_a;
    auto b = base;
    b.target_lang = cfg.lang_b;
    to_a_.configure(a);
    to_b_.configure(b);
}

void TwoWayTranslate::stop()
{
    to_a_.stop();
    to_b_.stop();
    std::lock_guard<std::mutex> lk(idle_mtx_);
    idle_detector_.reset(now_ms());
}

void TwoWayTranslate::push_input_pcm(const uint8_t *data, size_t len)
{
    to_a_.push_input_pcm(data, len);
    to_b_.push_input_pcm(data, len);

    const bool has_signal = s16le_rms(data, len) >= idle_threshold_rms_.load();
    const uint64_t now = now_ms();
    if (has_signal) last_signal_ms_ = now;
    std::lock_guard<std::mutex> lk(idle_mtx_);
    idle_detector_.feed(has_signal, now);
}

void TwoWayTranslate::set_output_active(bool active)
{
    output_active_.store(active);
}

LivePauseState TwoWayTranslate::pause_state()
{
    PauseInputs in;
    in.only_while_output = only_while_output_.load();
    in.output_active = output_active_.load();
    const bool output_blocked = in.only_while_output && !in.output_active;
    LivePauseState state;
    {
        std::lock_guard<std::mutex> lk(idle_mtx_);
        const uint64_t now = now_ms();
        // Explicit output resume gets a new idle window (spec 004).
        if (output_was_blocked_ && !output_blocked) idle_detector_.reset(now);
        output_was_blocked_ = output_blocked;
        in.idle = idle_detector_.poll(now);
        state.waiting_for_sound = idle_detector_.awaiting_signal();
    }
    state.reason = resolve_pause(in);
    if (state.reason != PauseReason::Idle) state.waiting_for_sound = false;
    return state;
}

void TwoWayTranslate::check_deaf(LiveTranslateSession &session, uint64_t &last_kick,
                                 uint64_t signal_ms, uint64_t now)
{
    const uint64_t since = session.connected_since_ms();
    if (!since) return;
    // Only judge while the mic is audible right now: silence legitimately
    // produces no text. Music without speech can trip this too; a reconnect
    // then costs well under a second and the queued audio is kept.
    if (!signal_ms || elapsed_ms(signal_ms, now) > kAudibleWindowMs) return;
    const uint64_t ref = std::max({session.last_input_ms(), session.last_output_ms(), since, last_kick});
    if (elapsed_ms(ref, now) < kDeafMs) return;
    last_kick = now;
    blog(LOG_WARNING, "[live-translate] two-way %s deaf: mic audible but no transcription for "
         "%llus; reconnecting", session.label().c_str(),
         static_cast<unsigned long long>(kDeafMs / 1000));
    session.request_rotate();
}

void TwoWayTranslate::watchdog()
{
    const uint64_t now = now_ms();
    const uint64_t since_a = to_a_.connected_since_ms(), since_b = to_b_.connected_since_ms();
    const uint64_t in = std::max(to_a_.last_input_ms(), to_b_.last_input_ms());
    const uint64_t out = std::max(to_a_.last_output_ms(), to_b_.last_output_ms());

    std::lock_guard<std::mutex> lk(watchdog_mtx_);
    const uint64_t signal = last_signal_ms_.load();
    check_deaf(to_a_, last_deaf_kick_a_, signal, now);
    check_deaf(to_b_, last_deaf_kick_b_, signal, now);

    // Every utterance is in language A or B, so one of the two sessions should
    // translate it. Arm on recognized speech that no output has followed;
    // disarm as soon as output catches up, or while either side is offline.
    if (!since_a || !since_b || in == 0 || out >= in) {
        pending_since_ = 0;
        return;
    }
    // A long pause in the speech restarts the window, so a lone filler word
    // never triggers it; film-like dialogue with short gaps still counts.
    if (elapsed_ms(in, now) > kSpeakingWindowMs) {
        pending_since_ = 0;
        return;
    }
    if (!pending_since_) pending_since_ = now;
    if (elapsed_ms(pending_since_, now) < kStallMs) return;
    if (last_kick_ms_ && elapsed_ms(last_kick_ms_, now) < kStallCooldownMs) return;

    last_kick_ms_ = now;
    pending_since_ = 0;
    blog(LOG_WARNING, "[live-translate] two-way stall: speech recognized for %llus without a "
         "translation; reconnecting both sessions",
         static_cast<unsigned long long>(kStallMs / 1000));
    to_a_.request_rotate();
    to_b_.request_rotate();
}

bool TwoWayTranslate::is_running() const
{
    return to_a_.is_running() || to_b_.is_running();
}

bool TwoWayTranslate::has_auth_error()
{
    return to_a_.status() == ConnStatus::AuthError || to_b_.status() == ConnStatus::AuthError;
}

std::string TwoWayTranslate::status_text()
{
    std::string a = to_a_.status_text(), b = to_b_.status_text();
    if (a == b) return a;
    std::lock_guard<std::mutex> lk(cfg_mtx_);
    return "to " + lang_a_ + ": " + a + " / to " + lang_b_ + ": " + b;
}

}
