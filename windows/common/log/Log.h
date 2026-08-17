#pragma once

#include <string>

namespace phonecam::log {

enum class Level { Info, Warn, Error };

void Write(Level level, const std::string& message);

inline void Info(const std::string& message) { Write(Level::Info, message); }
inline void Warn(const std::string& message) { Write(Level::Warn, message); }
inline void Error(const std::string& message) { Write(Level::Error, message); }

}  // namespace phonecam::log
