/*
 * Minimal standard-tuning bridge for the Apache-2.0 MSFA DX7 core.
 * The Classic Player currently exposes equal temperament; Scala/MTS support
 * can be added later without changing the FM voice implementation.
 */
#pragma once
#include <cstdint>
#include <memory>
class TuningState {
public:
    virtual ~TuningState() = default;
    virtual int32_t midinote_to_logfreq(int midinote) = 0;
    virtual bool is_standard_tuning() { return true; }
};
std::shared_ptr<TuningState> createStandardTuning();
