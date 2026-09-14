#pragma once
#include <string>

// Independent of rendering: deterministic dwell / grace / drag behavior.
struct VrMenuHoverHelp {
    std::string shown, candidate;
    float dwell = 0, away = 0;
    void Reset() { *this = {}; }
    void Advance(float dt, const std::string& hovered, bool dragging) {
        if (dragging) { candidate.clear(); dwell = away = 0; return; }
        if (hovered.empty()) {
            away += dt;
            if (away >= 0.25F) { shown.clear(); candidate.clear(); dwell = 0; }
            return;
        }
        away = 0;
        if (hovered == shown) { candidate.clear(); dwell = 0; return; }
        if (candidate != hovered) { candidate = hovered; dwell = 0; }
        dwell += dt;
        if (dwell >= 0.60F) { shown = hovered; candidate.clear(); dwell = 0; }
    }
};
