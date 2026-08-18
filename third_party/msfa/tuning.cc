#include "tuning.h"
#include <cmath>
namespace {
class StandardTuning final : public TuningState {
public:
    int32_t midinote_to_logfreq(int note) override {
        return (int32_t) std::llround((std::log(440.0) / std::log(2.0)
             + ((double) note - 69.0) / 12.0) * (double) (1 << 24));
    }
};
}
std::shared_ptr<TuningState> createStandardTuning() {
    return std::make_shared<StandardTuning>();
}
