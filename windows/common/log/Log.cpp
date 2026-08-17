#include "log/Log.h"

#include <cstdio>

#include <windows.h>

namespace phonecam::log {

namespace {

const char* LevelTag(Level level) {
    switch (level) {
        case Level::Info:
            return "INFO";
        case Level::Warn:
            return "WARN";
        case Level::Error:
            return "ERROR";
    }
    return "?";
}

}  // namespace

// Writes to both stderr (visible when phonecam-host runs in a console) and
// OutputDebugString (the only channel available when phonecam-vcam.dll is
// loaded into another process's address space, e.g. Zoom's, where stderr
// isn't attached to anything we can read).
void Write(Level level, const std::string& message) {
    const std::string line = std::string("[phonecam][") + LevelTag(level) + "] " + message + "\n";
    std::fputs(line.c_str(), stderr);
    OutputDebugStringA(line.c_str());
}

}  // namespace phonecam::log
