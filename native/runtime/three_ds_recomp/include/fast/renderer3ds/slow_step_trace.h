#pragma once

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string_view>

namespace Fast::Renderer3ds {

// Opt-in attribution of slow renderer steps: TRIAEVUM_SLOW_STEP_MS=<threshold>
// prints every timed step that takes longer. Disabled, a step costs one load.
[[nodiscard]] inline double SlowStepThresholdMilliseconds() noexcept {
    static const double threshold = [] {
        const char* value = std::getenv("TRIAEVUM_SLOW_STEP_MS");
        return value != nullptr && *value != '\0' ? std::atof(value) : -1.0;
    }();
    return threshold;
}

// Successive marks attribute the time since the previous mark (or the start)
// to the named step.
class SlowStepTrace {
public:
    explicit SlowStepTrace(std::string_view scope) noexcept
        : mScope(scope), mEnabled(SlowStepThresholdMilliseconds() >= 0.0) {
        if (mEnabled) mLast = std::chrono::steady_clock::now();
    }

    void Mark(std::string_view step) noexcept {
        if (!mEnabled) return;
        const auto now = std::chrono::steady_clock::now();
        const double milliseconds = std::chrono::duration<double, std::milli>(now - mLast).count();
        if (milliseconds > SlowStepThresholdMilliseconds())
            std::fprintf(stderr, "TRIAEVUM_SLOW_STEP scope=%.*s step=%.*s ms=%.1f\n",
                         static_cast<int>(mScope.size()), mScope.data(),
                         static_cast<int>(step.size()), step.data(), milliseconds);
        mLast = std::chrono::steady_clock::now();
    }

private:
    std::string_view mScope;
    bool mEnabled;
    std::chrono::steady_clock::time_point mLast{};
};

// One step covering the scope's lifetime.
class SlowStepScope {
public:
    explicit SlowStepScope(std::string_view step) noexcept : mTrace(step) {}
    ~SlowStepScope() { mTrace.Mark("total"); }
    SlowStepScope(const SlowStepScope&) = delete;
    SlowStepScope& operator=(const SlowStepScope&) = delete;

private:
    SlowStepTrace mTrace;
};

} // namespace Fast::Renderer3ds
