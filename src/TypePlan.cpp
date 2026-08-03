#include "TypePlan.h"

#include <algorithm>

namespace clipp {

namespace {
bool Contains(const std::vector<TypeKeyCode>& keys, TypeKeyCode key) {
    return std::find(keys.begin(), keys.end(), key) != keys.end();
}
}  // namespace

TypeSchedule BuildTypeSchedule(const TypePlan& plan) {
    TypeSchedule schedule;
    schedule.characterCount = plan.characterCount;
    schedule.enterCount = plan.enterCount;

    std::vector<TypeKeyCode> held;
    for (const KeyChord& chord : plan.chords) {
        // Release held modifiers this chord doesn't want (newest first).
        for (std::size_t i = held.size(); i-- > 0;) {
            if (!Contains(chord.modifiers, held[i])) {
                schedule.events.push_back(TypeKeyEvent{ held[i], false });
                held.erase(held.begin() + static_cast<std::ptrdiff_t>(i));
            }
        }
        // Press the ones it wants that aren't down yet.
        for (const TypeKeyCode modifier : chord.modifiers) {
            if (!Contains(held, modifier)) {
                schedule.events.push_back(TypeKeyEvent{ modifier, true });
                held.push_back(modifier);
            }
        }
        schedule.events.push_back(TypeKeyEvent{ chord.key, true });
        schedule.events.push_back(TypeKeyEvent{ chord.key, false });
    }
    // Anything still held at the end comes back up (newest first).
    for (std::size_t i = held.size(); i-- > 0;) {
        schedule.events.push_back(TypeKeyEvent{ held[i], false });
    }
    return schedule;
}

int EstimateTypeSeconds(const TypeSchedule& schedule) {
    const long long millis =
        static_cast<long long>(schedule.events.size()) * kTypeEventIntervalMs;
    return static_cast<int>((millis + 999) / 1000);
}

}  // namespace clipp
