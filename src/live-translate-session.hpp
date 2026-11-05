#pragma once
#include "caption-session.hpp" // ConnStatus
#include "live-caption-buffer.hpp"
#include "pause-policy.hpp"
#include "ring-buffer.hpp"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

// Two-way caption engine: two WebSockets to gemini-3.5-live-translate-preview,
// one per target language, both fed the same microphone audio. With
// echoTargetLanguage = false each session stays silent for speech already in
// its target language, so Japanese speech only produces English captions and
// English speech only Japanese captions (or any other language pair).
// Spec: docs/specs/007-two-way-live-translate.md.
namespace lt {

// What a session's worker needs to know about auto-pause (spec 004 rules).
struct LivePauseState {
    PauseReason reason = PauseReason::None;
    bool waiting_for_sound = false; // idle pause that has never heard a signal
};

// One live-translate WebSocket for one target language: reconnect with backoff,
// proactive rotation, a shared pause gate and its own caption display state.
class LiveTranslateSession {
public:
    using TextSink = std::function<void(const std::string &text)>;
    using PauseGate = std::function<LivePauseState()>;

    struct Config {
        std::string api_key;
        std::string target_lang; // BCP-47 code
        int max_lines = 2;
        int max_width = 60;
        uint64_t hold_ms = 4000;
    };

    // `label` only prefixes log lines; `gate` is polled every tick; `on_tick`
    // (may be empty) runs on the worker after every display tick.
    LiveTranslateSession(std::string label, PauseGate gate, std::function<void()> on_tick = {});
    ~LiveTranslateSession();
    LiveTranslateSession(const LiveTranslateSession &) = delete;
    LiveTranslateSession &operator=(const LiveTranslateSession &) = delete;

    // caption receives translated text; transcript (may be empty) receives the
    // input transcription. Replaces previous sinks and republishes.
    void set_sinks(TextSink caption, TextSink transcript);

    // Starts the worker, or applies a change: a new key or target language
    // reconnects; box and hold settings apply live. Empty api_key stops.
    void configure(const Config &cfg);
    void stop(); // joins the worker, then blanks both sinks

    void push_input_pcm(const uint8_t *data, size_t len); // 16 kHz mono S16LE

    ConnStatus status();
    std::string status_text();
    bool is_running() const { return running_.load(); }

    // Watchdog inputs (steady-clock ms, 0 = never / not connected).
    uint64_t last_input_ms() const { return last_input_ms_.load(); }
    uint64_t last_output_ms() const { return last_output_ms_.load(); }
    uint64_t connected_since_ms() const { return connected_since_.load(); }
    // Reconnects the current connection without a backoff wait.
    void request_rotate() { rotate_requested_ = true; }
    const std::string &label() const { return label_; }

private:
    void run();
    void tick();    // hold expiry + publish
    void publish(); // renders both buffers; calls sinks only on change
    bool wait_while_paused(LivePauseState state); // false = stopping
    void announce_pause(const LivePauseState &state);
    void set_status(ConnStatus s, const std::string &detail = "");

    static constexpr uint64_t kSessionMaxMs = 9 * 60 * 1000;
    static constexpr uint64_t kTickMs = 100;
    static constexpr size_t kChunkBytes = 3200;
    static constexpr size_t kResumePrerollBytes = 16000 * 2 * 1;
    static constexpr uint64_t kStatsMs = 60 * 1000;

    const std::string label_;
    const PauseGate gate_;
    const std::function<void()> on_tick_;
    ByteRingBuffer input_{16000 * 2 * 5};

    std::mutex lifecycle_mtx_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> config_changed_{false};
    // True once the server accepted a setup with the current key/target. Until
    // then a rejection stops the session; afterwards every failure reconnects.
    std::atomic<bool> proven_{false};
    std::atomic<bool> rotate_requested_{false};

    std::atomic<uint64_t> last_input_ms_{0};
    std::atomic<uint64_t> last_output_ms_{0};
    std::atomic<uint64_t> connected_since_{0};
    std::atomic<uint64_t> messages_{0};      // any server message, audio included
    std::atomic<uint64_t> input_deltas_{0};  // since the last stats line
    std::atomic<uint64_t> output_deltas_{0};
    uint64_t last_stats_ms_ = 0;              // worker thread only

    // State lock: config, buffers, sinks. Sinks are called without it, under
    // publish_mtx_ (lock order publish_mtx_ -> mtx_).
    std::mutex mtx_;
    std::mutex publish_mtx_;
    Config cfg_;
    LiveCaptionBuffer caption_;
    LiveCaptionBuffer transcript_;
    TextSink caption_sink_;
    TextSink transcript_sink_;
    std::string shown_caption_;
    std::string shown_transcript_;

    std::string announced_pause_; // worker thread only

    std::mutex status_mtx_;
    ConnStatus status_ = ConnStatus::Idle;
    std::string status_detail_;
};

struct TwoWayConfig {
    std::string api_key;
    std::string lang_a = "ja"; // captions_a shows translations INTO lang_a
    std::string lang_b = "en"; // captions_b shows translations INTO lang_b
    int max_lines = 2;
    int max_width = 60;
    double hold_seconds = 4.0;
    int idle_timeout_seconds = 300;
    double idle_threshold_dbfs = -45.0;
    bool only_while_output_active = false;
};

// Plugin-wide owner of the two sessions and of the auto-pause state they share.
class TwoWayTranslate {
public:
    using TextSink = LiveTranslateSession::TextSink;

    static TwoWayTranslate &instance();

    // transcript (may be empty) shows what was recognized, from session A.
    void set_sinks(TextSink caption_a, TextSink caption_b, TextSink transcript);
    void configure(const TwoWayConfig &cfg);
    void stop();

    void push_input_pcm(const uint8_t *data, size_t len);
    void set_output_active(bool active);

    bool is_running() const;
    bool has_auth_error();
    std::string status_text();

private:
    TwoWayTranslate();
    ~TwoWayTranslate();
    LivePauseState pause_state(); // called from both workers
    // Watchdogs, run after each worker tick (spec 007 §4.6):
    //  - deaf: the mic is audible but one session has sent no text for kDeafMs
    //    (seen live: after a rotation the server accepted audio and went silent)
    //    -> reconnect that session, at most once per kDeafMs.
    //  - stall: speech keeps being recognized for kStallMs (gaps up to
    //    kSpeakingWindowMs) while neither direction translates -> reconnect
    //    both, at most once per kStallCooldownMs.
    void watchdog();
    void check_deaf(LiveTranslateSession &session, uint64_t &last_kick, uint64_t signal_ms,
                    uint64_t now);
    static constexpr uint64_t kDeafMs = 45 * 1000;
    static constexpr uint64_t kAudibleWindowMs = 2 * 1000;
    static constexpr uint64_t kStallMs = 30 * 1000;
    static constexpr uint64_t kStallCooldownMs = 60 * 1000;
    static constexpr uint64_t kSpeakingWindowMs = 10 * 1000;
    std::mutex watchdog_mtx_;
    uint64_t pending_since_ = 0;  // first recognized speech not yet followed by output
    uint64_t last_kick_ms_ = 0;
    uint64_t last_deaf_kick_a_ = 0;
    uint64_t last_deaf_kick_b_ = 0;
    std::atomic<uint64_t> last_signal_ms_{0}; // last mic chunk at or above the idle threshold

    std::mutex idle_mtx_; // leaf lock
    IdleDetector idle_detector_;
    bool output_was_blocked_ = false;
    std::atomic<double> idle_threshold_rms_{0.0};
    std::atomic<bool> only_while_output_{false};
    std::atomic<bool> output_active_{false};

    std::mutex cfg_mtx_;
    std::string lang_a_ = "ja";
    std::string lang_b_ = "en";

    LiveTranslateSession to_a_;
    LiveTranslateSession to_b_;
};

}
