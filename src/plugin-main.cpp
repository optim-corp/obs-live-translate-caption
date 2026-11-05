#include "caption-session.hpp"
#include "live-translate-session.hpp"
#include "output-state.hpp"
#include <obs-module.h>

extern struct obs_source_info live_translate_filter_info;

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-live-translate-caption", "en-US")

extern "C" const char *obs_module_name(void) { return "OBS Live Translate Caption"; }
extern "C" const char *obs_module_description(void)
{
    return "Real-time translated captions via the Gemini API.";
}

bool obs_module_load(void)
{
    blog(LOG_INFO, "[live-translate] module loaded");
    obs_register_source(&live_translate_filter_info);
    // Hooks the frontend events and seeds the session with the current
    // streaming/recording/virtualcam state (spec 004 §4.5).
    lt::output_state_init();
    return true;
}

void obs_module_unload(void)
{
    // Unhook the frontend before stopping: an output stop/start event arriving
    // mid-teardown would otherwise push state into a session we just stopped.
    lt::output_state_shutdown();
    // The caption session's sinks call back into libobs (text source updates).
    // Stop it and drop the sinks now, while libobs is still alive; the
    // singleton's destructor runs at process exit, after libobs is gone.
    auto &captions = lt::CaptionSession::instance();
    captions.stop();
    captions.set_sinks({}, {});
    auto &two_way = lt::TwoWayTranslate::instance();
    two_way.stop();
    two_way.set_sinks({}, {}, {});
    blog(LOG_INFO, "[live-translate] module unloaded");
}
