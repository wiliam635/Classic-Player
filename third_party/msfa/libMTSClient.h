/*
 * Minimal MTS bridge for the Apache-2.0 MSFA DX7 core.
 * Standard 12-tone tuning is used when no external tuning service is present.
 */
#pragma once
#include <cmath>
struct MTSClient {};
inline bool MTS_HasMaster(MTSClient*) { return false; }
inline double MTS_NoteToFrequency(MTSClient*, int note, int) {
    return 440.0 * std::pow(2.0, ((double) note - 69.0) / 12.0);
}
