#pragma once
// JSON between the page (ui/app.js) and the engine, and the settings file.
// Message shapes are the ones the page's mock engine produces, so the page
// behaves the same in a browser and in the host.
#include "engine/Engine.h"

#include <nlohmann/json.hpp>

#include <string>

namespace vela::app {

nlohmann::json settingsToJson(const core::Settings& settings);
// One "set" from the page: dotted path + value. False for an unknown path.
bool applySetting(core::Settings& settings, const std::string& path, const nlohmann::json& value);
core::Settings settingsFromJson(const nlohmann::json& json);

std::string stateMessage(const engine::EngineState& state);
std::string telemetryMessage(const engine::EngineTelemetry& telemetry);

core::Settings loadSettings();
void saveSettings(const core::Settings& settings);

} // namespace vela::app
