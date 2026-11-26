#pragma once
#include <stdint.h>
#include <stddef.h>
namespace gzeek {
enum class ScheduleView { unavailable, timed, fullPlan };
inline ScheduleView scheduleView(bool available, bool clockKnown) {
    return !available ? ScheduleView::unavailable :
           (clockKnown ? ScheduleView::timed : ScheduleView::fullPlan);
}

inline unsigned offlineMinute(uint32_t syncedSeconds, uint64_t elapsedUs) {
    return unsigned((syncedSeconds + elapsedUs / 1000000) % 86400) / 60;
}
struct BrowsePosition { unsigned room; unsigned event; };
inline BrowsePosition nextOfflineView(unsigned room, unsigned event,
                                      unsigned eventsInRoom, unsigned roomCount) {
    if (event + 2 < eventsInRoom) return {room, event + 2};
    return {(room + 1) % roomCount, 0};
}

inline uint32_t checksum(const char* bytes, size_t count) {
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < count; ++i) { hash ^= uint8_t(bytes[i]); hash *= 16777619u; }
    return hash;
}
inline bool newerGeneration(uint32_t candidate, uint32_t previous) {
    return int32_t(candidate - previous) > 0;
}
inline const char* audioFailure(bool wifi, bool mqtt, bool requested, uint32_t now,
                                uint32_t requestedAt, uint32_t lastPacketAt,
                                uint32_t requestTimeout, uint32_t chunkTimeout,
                                uint32_t totalTimeout) {
    if (!wifi) return "No Wi-Fi connection";
    if (!mqtt) return "MQTT unavailable";
    if (uint32_t(now-requestedAt) >= totalTimeout) return "Audio total timeout";
    if (requested && uint32_t(now-requestedAt) >= requestTimeout) return "Audio request timeout";
    if (!requested && uint32_t(now-lastPacketAt) >= chunkTimeout) return "Audio stream timeout";
    return nullptr;
}
}
