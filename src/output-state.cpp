#include "output-state.hpp"
#include "caption-session.hpp"
#include "live-translate-session.hpp"
#include <atomic>
#include <obs-frontend-api.h>
#include <obs-module.h>

// OBS output state via obs-frontend-api (spec 004 §4.5, §4.6).
namespace lt {
namespace {

// Written from the frontend (UI) thread, read from the session's worker.
std::atomic<bool> g_active{false};

// obs_frontend_* return false when no frontend is present (libobs-only hosts),
// which is exactly the "nothing is running" answer we want there.
bool query_output_active()
{
    return obs_frontend_streaming_active() || obs_frontend_recording_active() ||
           obs_frontend_virtualcam_active();
}

// Pushes the current OR into the session; logs only on a change so a long
// stream does not spam the log. Returns nothing: the session polls the value.
void publish_output_active()
{
    bool active = query_output_active();
    bool was = g_active.exchange(active);
    CaptionSession::instance().set_output_active(active);
    TwoWayTranslate::instance().set_output_active(active);
    if (was != active)
        blog(LOG_INFO, "[live-translate] output active: %s",
             active ? "true" : "false");
}

// Runs on the UI thread for every frontend event, so it must stay cheap: three
// bool queries and an atomic store for the handful of events we care about.
void on_frontend_event(enum obs_frontend_event event, void * /*data*/)
{
    switch (event) {
    case OBS_FRONTEND_EVENT_STREAMING_STARTED:
    case OBS_FRONTEND_EVENT_STREAMING_STOPPED:
    case OBS_FRONTEND_EVENT_RECORDING_STARTED:
    case OBS_FRONTEND_EVENT_RECORDING_STOPPED:
    case OBS_FRONTEND_EVENT_VIRTUALCAM_STARTED:
    case OBS_FRONTEND_EVENT_VIRTUALCAM_STOPPED:
    // Modules load before the frontend finishes starting up, so the value read
    // in output_state_init() can predate a restored/autostarted output.
    case OBS_FRONTEND_EVENT_FINISHED_LOADING:
        publish_output_active();
        break;
    default:
        break;
    }
}

} // namespace

void output_state_init()
{
    obs_frontend_add_event_callback(on_frontend_event, nullptr);
    // Seed the session before the first event arrives; a module reload while
    // OBS is already streaming would otherwise stay "inactive" until it stops.
    g_active.store(query_output_active());
    CaptionSession::instance().set_output_active(g_active.load());
    TwoWayTranslate::instance().set_output_active(g_active.load());
}

void output_state_shutdown()
{
    obs_frontend_remove_event_callback(on_frontend_event, nullptr);
}

bool output_state_active() { return g_active.load(); }

}
