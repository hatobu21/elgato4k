#pragma once
#include <string_view>

namespace vela::log {

// Buffered, thread-safe file log at %LOCALAPPDATA%\Vela\logs\vela.log.
// Lines go to the debugger as well. Never call per frame without a guard:
// the frame path logs only failures and state changes.
void open();
void close();
void info(std::string_view tag, std::string_view message);
void warn(std::string_view tag, std::string_view message);
void error(std::string_view tag, std::string_view message);
void flush();

} // namespace vela::log
