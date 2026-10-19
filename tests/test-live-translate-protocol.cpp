#include <catch2/catch_test_macros.hpp>
#include "live-translate-protocol.hpp"
#include <nlohmann/json.hpp>

using namespace lt;
using nlohmann::json;

TEST_CASE("live translate setup uses the preview model with AUDIO output")
{
    json j = json::parse(build_live_translate_setup_message("en", false));
    REQUIRE(j["setup"]["model"] == "models/gemini-3.5-live-translate-preview");
    const auto &gc = j["setup"]["generationConfig"];
    REQUIRE(gc["responseModalities"] == json::array({"AUDIO"}));
}

TEST_CASE("live translate setup enables both transcriptions on setup, not generationConfig")
{
    // The server rejects them inside generationConfig (close 1007, "Unknown name").
    json j = json::parse(build_live_translate_setup_message("ja", false));
    const auto &setup = j["setup"];
    REQUIRE(setup["inputAudioTranscription"].is_object());
    REQUIRE(setup["inputAudioTranscription"].empty());
    REQUIRE(setup["outputAudioTranscription"].is_object());
    REQUIRE(setup["outputAudioTranscription"].empty());
    REQUIRE_FALSE(setup["generationConfig"].contains("inputAudioTranscription"));
    REQUIRE_FALSE(setup["generationConfig"].contains("outputAudioTranscription"));
}

TEST_CASE("live translate setup carries the target language and echo flag")
{
    json off = json::parse(build_live_translate_setup_message("ja", false));
    REQUIRE(off["setup"]["generationConfig"]["translationConfig"]["targetLanguageCode"] == "ja");
    REQUIRE(off["setup"]["generationConfig"]["translationConfig"]["echoTargetLanguage"] == false);

    json on = json::parse(build_live_translate_setup_message("pt-BR", true));
    REQUIRE(on["setup"]["generationConfig"]["translationConfig"]["targetLanguageCode"] == "pt-BR");
    REQUIRE(on["setup"]["generationConfig"]["translationConfig"]["echoTargetLanguage"] == true);
}

TEST_CASE("live translate parser reads input and output transcriptions with language codes")
{
    auto m = parse_live_translate_server_message(R"({"serverContent":{
        "inputTranscription":{"text":"こんにちは","languageCode":"ja"},
        "outputTranscription":{"text":" Hello","languageCode":"en"}}})");
    REQUIRE(m.input_text == "こんにちは");
    REQUIRE(m.input_language == "ja");
    REQUIRE(m.output_text == " Hello");
    REQUIRE(m.output_language == "en");
    REQUIRE_FALSE(m.turn_complete);
    REQUIRE_FALSE(m.error);
}

TEST_CASE("live translate parser reports turnComplete")
{
    auto m = parse_live_translate_server_message(R"({"serverContent":{"turnComplete":true}})");
    REQUIRE(m.turn_complete);
    REQUIRE(m.output_text.empty());
}

TEST_CASE("live translate parser ignores audio-only model turns")
{
    auto m = parse_live_translate_server_message(R"({"serverContent":{"modelTurn":{"parts":[
        {"inlineData":{"mimeType":"audio/pcm;rate=24000","data":"AAAA"}}]}}})");
    REQUIRE(m.input_text.empty());
    REQUIRE(m.output_text.empty());
    REQUIRE_FALSE(m.turn_complete);
    REQUIRE_FALSE(m.setup_complete);
}

TEST_CASE("live translate parser reports setupComplete, goAway and errors")
{
    REQUIRE(parse_live_translate_server_message(R"({"setupComplete":{}})").setup_complete);
    REQUIRE(parse_live_translate_server_message(R"({"goAway":{"timeLeft":"10s"}})").go_away);
    auto e = parse_live_translate_server_message(R"({"error":{"code":400,"message":"bad model"}})");
    REQUIRE(e.error);
    REQUIRE(e.error_message == "bad model");
}

TEST_CASE("live translate parser tolerates malformed input")
{
    auto bad = parse_live_translate_server_message("{not json");
    REQUIRE_FALSE(bad.error);
    REQUIRE(bad.output_text.empty());
    auto wrong = parse_live_translate_server_message(
        R"({"serverContent":{"outputTranscription":{"text":42},"turnComplete":"yes"}})");
    REQUIRE(wrong.output_text.empty());
    REQUIRE_FALSE(wrong.turn_complete);
    REQUIRE(parse_live_translate_server_message("[1,2]").output_text.empty());
}
