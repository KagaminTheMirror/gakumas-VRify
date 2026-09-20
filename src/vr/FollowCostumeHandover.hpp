#pragma once

#include <string>
#include <string_view>

namespace gakumas::vr {

// GameObject names look like "hski | CampusActorController[0]". Official
// GetCharacterId is the same four-letter id without the suffix.
inline std::string CharacterIdFromActorName(std::string_view name) noexcept {
    const auto cut = name.find(" |");
    if (cut != std::string_view::npos) return std::string(name.substr(0, cut));
    return std::string(name);
}

struct CostumeHandoverSample {
    int followIndex = -1;
    std::string followCharacterId;
    int previousOfficialIndex = -1;
    std::string previousOfficialCharacterId;
    int officialIndex = -1;
    std::string officialCharacterId;
};

struct CostumeHandoverDecision {
    bool shouldSwitch = false;
    int fromIndex = -1;
    int toIndex = -1;
};

// Official clothes change: focus index moved between two models that share
// a character id, and FOLLOW is still on the outgoing official model.
// Different idols, first observation, repeats, and a user who already
// Y-cycled to the incoming model do not switch.
inline CostumeHandoverDecision DecideCostumeHandover(
    const CostumeHandoverSample& sample) noexcept {
    CostumeHandoverDecision decision;
    decision.fromIndex = sample.followIndex;
    decision.toIndex = sample.officialIndex;
    if (sample.followIndex < 0 || sample.officialIndex < 0 ||
        sample.previousOfficialIndex < 0) {
        return decision;
    }
    if (sample.officialIndex == sample.previousOfficialIndex) return decision;
    if (sample.followIndex != sample.previousOfficialIndex) return decision;
    if (sample.followIndex == sample.officialIndex) return decision;
    if (sample.followCharacterId.empty() ||
        sample.officialCharacterId.empty() ||
        sample.previousOfficialCharacterId.empty()) {
        return decision;
    }
    if (sample.followCharacterId != sample.officialCharacterId) return decision;
    if (sample.previousOfficialCharacterId != sample.officialCharacterId) {
        return decision;
    }
    decision.shouldSwitch = true;
    return decision;
}

void TickFollowCostumeHandover() noexcept;
void NoteFollowCostumeActor(void* actor, int index) noexcept;

} // namespace gakumas::vr
