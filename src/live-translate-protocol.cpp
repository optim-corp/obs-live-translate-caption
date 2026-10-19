#include "live-translate-protocol.hpp"
#include <nlohmann/json.hpp>

using nlohmann::json;

namespace lt {

namespace {

void read_transcription(const json &sc, const char *key, std::string &text, std::string &language)
{
    if (!sc.contains(key) || !sc[key].is_object()) return;
    const auto &t = sc[key];
    if (t.contains("text") && t["text"].is_string()) text = t["text"].get<std::string>();
    if (t.contains("languageCode") && t["languageCode"].is_string())
        language = t["languageCode"].get<std::string>();
}

} // namespace

std::string build_live_translate_setup_message(const std::string &target_language_code,
                                               bool echo_target_language)
{
    // Field placement follows the google-genai SDK's MLDev converter: the
    // transcription configs sit on setup itself (the docs' raw-JSON sample
    // nests them in generationConfig, which the server rejects with 1007).
    json gc;
    gc["responseModalities"] = json::array({"AUDIO"});
    gc["translationConfig"]["targetLanguageCode"] = target_language_code;
    gc["translationConfig"]["echoTargetLanguage"] = echo_target_language;
    json j;
    j["setup"]["model"] = kLiveTranslateModel;
    j["setup"]["generationConfig"] = gc;
    j["setup"]["inputAudioTranscription"] = json::object();
    j["setup"]["outputAudioTranscription"] = json::object();
    return j.dump();
}

LiveTranslateServerMessage parse_live_translate_server_message(const std::string &json_text)
{
    LiveTranslateServerMessage m;
    json j = json::parse(json_text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return m;

    if (j.contains("error") && j["error"].is_object()) {
        m.error = true;
        m.error_message = j["error"].value("message", "");
        return m;
    }
    m.setup_complete = j.contains("setupComplete");
    m.go_away = j.contains("goAway");

    if (j.contains("serverContent") && j["serverContent"].is_object()) {
        const auto &sc = j["serverContent"];
        read_transcription(sc, "inputTranscription", m.input_text, m.input_language);
        read_transcription(sc, "outputTranscription", m.output_text, m.output_language);
        m.turn_complete = sc.contains("turnComplete") && sc["turnComplete"].is_boolean() &&
                          sc["turnComplete"].get<bool>();
    }
    return m;
}

}
