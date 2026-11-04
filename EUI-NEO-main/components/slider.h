#pragma once

#include "components/theme.h"
#include "core/dsl.h"
#include "eui/signal.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <string>
#include <unordered_map>
#include <utility>

namespace components {

// [BUILD m] Built-in double-click-to-restore-default for EVERY slider. The detection state is keyed by
// slider id in ONE shared map (inline fn -> a single definition across TUs). No per-slider glue is needed:
// a slider opts in by calling .default(norm) with its semantic default as a normalized value in [0,1].
// Without that call the slider has no double-click behavior.
//   - Double-click = two pointer releases within kDcTimeMs whose RAW normalized values differ < kDcDiff.
//   - The raw value (nv in [0,1]) is recorded in onChange BEFORE any level rounding, so a slider whose
//     levels are {0,50,100}% that you tap at 20% records 0.20, not the rounded 0%.
//   - Detection runs on the .hit onRelease hook (BUILD k): one drag = one release => cannot fake a
//     double-click; two quick taps at the same spot = two releases with the same raw value => exact hit.
namespace slider_internal {
struct SliderDcState { float curNorm = -1.0f; long long curMs = 0; float prevNorm = -1.0f; long long prevMs = 0; };
constexpr float kDcDiff = 0.03f;        // 3% of slider width
constexpr long long kDcTimeMs = 300LL;
inline std::unordered_map<std::string, SliderDcState>& dcMap() {
    static std::unordered_map<std::string, SliderDcState> m;
    return m;
}
inline void dcRecord(const std::string& id, float nv) {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    auto& s = dcMap()[id];
    s.curNorm = nv;
    s.curMs = ms;
}
inline bool dcRelease(const std::string& id) {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    auto& s = dcMap()[id];
    bool hit = false;
    if (s.prevNorm >= 0.0f) {   // prevNorm < 0 => no previous gesture yet, do not flag double-click
        const float d = std::fabs(s.curNorm - s.prevNorm);
        const long long dt = ms - s.prevMs;   // ms = this release; prevMs = previous release
        if (d < kDcDiff && dt < kDcTimeMs) hit = true;
    }
    s.prevNorm = s.curNorm;   // this gesture becomes the "previous" one
    s.prevMs = ms;            // use this release's real time, avoid stale time on a 2nd click with no onChange
    return hit;
}
} // namespace slider_internal

struct SliderStyle {
    SliderStyle() : SliderStyle(theme::dark()) {}

    explicit SliderStyle(const theme::ThemeColorTokens& tokens) {
        track = core::mixColor(tokens.surfaceHover, tokens.surfaceActive, tokens.dark ? 0.24f : 0.18f);
        fill = tokens.primary;
        knob = tokens.text;
    }

    core::Color track;
    core::Color fill;
    core::Color knob;
};

class SliderBuilder {
public:
    SliderBuilder(core::dsl::Ui& ui, std::string id)
        : ui_(ui), id_(std::move(id)) {}

    SliderBuilder& size(float width, float height) { width_ = width; height_ = height; return *this; }
    SliderBuilder& value(float value) { value_ = std::clamp(value, 0.0f, 1.0f); return *this; }
    SliderBuilder& bind(eui::Signal<float>& signal) {
        value(signal.get());
        onChange([&signal](float value) { signal.set(value); });
        return *this;
    }
    SliderBuilder& style(const SliderStyle& value) { style_ = value; return *this; }
    SliderBuilder& theme(const theme::ThemeColorTokens& tokens) {
        style_ = SliderStyle(tokens);
        metrics_ = tokens.metrics;
        return *this;
    }
    SliderBuilder& transition(const core::Transition& value) { transition_ = value; return *this; }
    SliderBuilder& transition(float duration, core::Ease ease = core::Ease::OutCubic) {
        transition_ = core::Transition::make(duration, ease);
        return *this;
    }
    SliderBuilder& onChange(std::function<void(float)> callback) { onChange_ = std::move(callback); return *this; }
    // [BUILD k] release hook: fires when the pointer is released over the slider's interactive hit area
    // (drag end OR a plain click). Used for "apply on release" so the page does not reflow mid-drag.
    SliderBuilder& onRelease(std::function<void()> callback) { onReleased_ = std::move(callback); return *this; }

    // [BUILD m] registers the slider's semantic default as a normalized value in [0,1].
    // Double-clicking the slider (two quick taps at ~same position) restores this default.
    // Without this call the slider has no double-click behavior.
    SliderBuilder& default(float norm) { defaultNorm_ = std::clamp(norm, 0.0f, 1.0f); return *this; }

    void build() {
        const float trackHeight = std::max(metrics_.spacing.tiny - metrics_.spacing.hairline,
                                           height_ * 0.18f);
        const float trackY = (height_ - trackHeight) * 0.5f;
        const float knobSize = std::max(metrics_.typography.label, height_ * 0.72f);
        const std::string sid = id_;
        const float defNorm = defaultNorm_;
        std::function<void(float)> userOnChange = onChange_;
        // [BUILD m] wrap the user callback so the RAW normalized value is recorded for double-click
        // detection BEFORE any level rounding the app may do in its own onChange.
        std::function<void(float)> onChange =
            [sid, userOnChange](float nv) {
                components::slider_internal::dcRecord(sid, nv);
                if (userOnChange) userOnChange(nv);
            };

        ui_.stack(id_)
            .size(width_, height_)
            .sliderState(id_, value_, width_, knobSize, onChange)
            .content([&] {
                ui_.rect(id_ + ".track")
                    .y(trackY)
                    .size(width_, trackHeight)
                    .color(style_.track)
                    .radius(trackHeight * 0.5f)
                    .build();

                ui_.rect(id_ + ".fill")
                    .y(trackY)
                    .size(width_ * value_, trackHeight)
                    .color(style_.fill)
                    .radius(trackHeight * 0.5f)
                    .transition(transition_)
                    .animate(core::AnimProperty::Color)
                    .sliderFillFrom(id_)
                    .build();

                ui_.rect(id_ + ".knob")
                    .y((height_ - knobSize) * 0.5f)
                    .size(knobSize, knobSize)
                    .color(style_.knob)
                    .radius(knobSize * 0.5f)
                    .shadow(12.0f, 0.0f, 4.0f, theme::withAlpha(style_.fill, 0.20f))
                    .transition(transition_)
                    .animate(core::AnimProperty::Color | core::AnimProperty::Shadow)
                    .sliderKnobFrom(id_)
                    .build();

        ui_.rect(id_ + ".hit")
            .size(width_, height_)
            .states(theme::color(0.0f, 0.0f, 0.0f, 0.0f),
                    theme::color(0.0f, 0.0f, 0.0f, 0.0f),
                    theme::color(0.0f, 0.0f, 0.0f, 0.0f))
            .zIndex(10)
            .interactive()
            .sliderInputFrom(id_)
            // [BUILD k] release hook: fires when the pointer is released over this slider's interactive
            // hit area (drag-end anywhere, or a plain click). Signature must match BuilderBase::onRelease
            // (void(const PointerEvent&, const Rect&)); we only forward to the user's void() callback.
            // [BUILD m] also runs double-click detection: two quick releases at ~same position restore the
            // configured default (defaultNorm_ >= 0) by re-firing the user's onChange with the default
            // value, then the user's own onRelease (commit) runs.
            .onRelease([cb = onReleased_, sid, defNorm, userOnChange](const auto&, const auto&) {
                if (defNorm >= 0.0f && components::slider_internal::dcRelease(sid)) {
                    if (userOnChange) userOnChange(defNorm);   // restore to default
                }
                if (cb) cb();
            })
            .build();
            })
            .build();
    }

private:
    core::dsl::Ui& ui_;
    std::string id_;
    SliderStyle style_;
    theme::ThemeMetricTokens metrics_;
    core::Transition transition_ = core::Transition::make(0.16f, core::Ease::OutCubic);
    std::function<void(float)> onChange_;
    std::function<void()> onReleased_;
    float defaultNorm_ = -1.0f;   // [BUILD m] semantic default (normalized); <0 => no double-click
    float width_ = 300.0f;
    float height_ = 32.0f;
    float value_ = 0.0f;
};

inline SliderBuilder slider(core::dsl::Ui& ui, const std::string& id) {
    return SliderBuilder(ui, id);
}

} // namespace components
