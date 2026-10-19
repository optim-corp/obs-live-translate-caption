#pragma once
#include <string>

// Live API message builders/parsers for the two-way engine, which talks to
// models/gemini-3.5-live-translate-preview (speech-to-speech translation). The
// translated speech itself is ignored; captions come from the transcriptions.
// Pure functions, no libobs dependency. Audio frames reuse
// build_realtime_input_message() from caption-protocol.hpp.
// Spec: docs/specs/007-two-way-live-translate.md §3, §4.1.
namespace lt {

constexpr const char *kLiveTranslateModel = "models/gemini-3.5-live-translate-preview";

// Builds the {"setup": {...}} JSON sent right after the WebSocket opens:
//   setup.model = kLiveTranslateModel
//   setup.generationConfig.responseModalities = ["AUDIO"] (the only modality
//     the model accepts)
//   setup.generationConfig.translationConfig = {targetLanguageCode, echoTargetLanguage}
//   setup.inputAudioTranscription = {}
//   setup.outputAudioTranscription = {}
// echo_target_language = false keeps the model silent for speech that is
// already in the target language; the two-way engine relies on that.
std::string build_live_translate_setup_message(const std::string &target_language_code,
                                               bool echo_target_language);

struct LiveTranslateServerMessage {
    bool setup_complete = false;
    bool turn_complete = false;     // serverContent.turnComplete
    bool go_away = false;           // {"goAway": {...}}: the server will close soon
    bool error = false;             // {"error": {...}}
    std::string error_message;
    std::string input_text;         // serverContent.inputTranscription.text (delta)
    std::string input_language;     // serverContent.inputTranscription.languageCode
    std::string output_text;        // serverContent.outputTranscription.text (delta)
    std::string output_language;    // serverContent.outputTranscription.languageCode
};

// Unparsable JSON and messages carrying only audio (modelTurn.parts[].inlineData)
// yield a message with every field at its default.
LiveTranslateServerMessage parse_live_translate_server_message(const std::string &json_text);

}
