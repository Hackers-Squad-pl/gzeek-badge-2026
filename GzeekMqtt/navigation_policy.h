#pragma once
#include <stdint.h>
namespace gzeek {
inline uint32_t roomDuration(uint32_t base, unsigned pages, uint32_t pageMs) {
    return base + (pages > 1 ? (pages - 1) * pageMs : 0);
}
inline bool mayAdvance(uint32_t now, uint32_t shownAt, uint32_t duration,
                       bool audioBusy, bool interacted, uint32_t lastClick,
                       uint32_t pause) {
    return !audioBusy && uint32_t(now - shownAt) >= duration &&
           (!interacted || uint32_t(now - lastClick) >= pause);
}
}
