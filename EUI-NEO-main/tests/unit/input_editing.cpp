#include "components/input.h"
#include <chrono>
#include <iostream>
#include <random>

int main() {
    using Model = components::input_detail::InputModel;
    using Clock = std::chrono::steady_clock;
    Model::InputState edited;
    edited.text = "first paragraph\nsecond paragraph with enough characters to wrap\n中文🙂末尾\n";
    std::mt19937 random(42);
    for (int i = 0; i < 150; ++i) {
        const int position = Model::clampUtf8Boundary(edited.text, static_cast<int>(random() % (edited.text.size() + 1)));
        edited.cursor = position;
        Model::clearSelection(edited);
        if (i % 3 == 0 && position < static_cast<int>(edited.text.size())) {
            edited.selectionEnd = Model::clampUtf8Boundary(edited.text, std::min(static_cast<int>(edited.text.size()), position + 9));
            Model::eraseSelection(edited);
        } else {
            Model::insertAtCursor(edited, i % 2 ? "中文\nnew line\n" : "🙂abcdef");
        }
        const float width = i % 7 ? 120.f : 180.f;
        Model::ensureLayoutCache(edited, "monospace", 16.f, width, true);
        const auto reference = Model::measureLines(edited.text, "monospace", 16.f, width);
        if (edited.cachedLines.size() != reference.size()) {
            std::cerr << "Incremental line count differs at edit " << i << "\n";
            return 1;
        }
        for (size_t j = 0; j < reference.size(); ++j) {
            const auto& actual = edited.cachedLines[j];
            const auto& expected = reference[j];
            if (actual.start != expected.start || actual.end != expected.end ||
                actual.hardBreakAfter != expected.hardBreakAfter || actual.metrics.caretX != expected.metrics.caretX ||
                actual.metrics.byteIndices != expected.metrics.byteIndices) {
                std::cerr << "Incremental metrics differ at edit " << i << ", line " << j << "\n";
                return 2;
            }
        }
    }
    Model::InputState composition;
    composition.text = "prefix suffix";
    Model::moveCursorTo(composition, 7, false);
    composition.compositionText = "中文测试";
    if (Model::displayState(composition, true).text != "prefix 中文测试suffix" || composition.text != "prefix suffix") return 3;
    Model::displayState(composition, false);
    if (composition.preedit) return 4;
    for (const int count : {2000, 20000}) {
        Model::InputState state;
        for (int i = 0; i < count; ++i)
            state.text += std::to_string(i) + " editable text with a moderately long line of content.\n";
        ++state.textRevision;
        const auto layout = [&] {
            Model::InputLayout::build(state, 480.f, 500.f, 500.f, 10.f, 10.f, 19.2f, "monospace", 16.f, true);
        };
        layout();
        const auto* prefixMetrics = state.cachedLines.front().metrics.caretX.data();
        const auto* suffixMetrics = state.cachedLines[state.cachedLines.size() - 2].metrics.caretX.data();
        state.cursor = Model::clampUtf8Boundary(state.text, static_cast<int>(state.text.size() / 2));
        Model::clearSelection(state);
        const auto start = Clock::now();
        for (int i = 0; i < 10; ++i) {
            Model::pushUndoState(state);
            Model::insertAtCursor(state, "x");
            layout();
            Model::pushUndoState(state);
            state.selectionStart = state.cursor - 1;
            state.selectionEnd = state.cursor;
            Model::eraseSelection(state);
            layout();
        }
        const auto milliseconds = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
        if (prefixMetrics != state.cachedLines.front().metrics.caretX.data() ||
            suffixMetrics != state.cachedLines[state.cachedLines.size() - 2].metrics.caretX.data()) {
            std::cerr << "Editing replaced untouched paragraph metrics\n";
            return 8;
        }
        std::cout << count << " lines: " << milliseconds / 20.0 << " ms/edit (middle insert/delete + layout + undo snapshot)" << std::endl;
        const auto compositionStart = Clock::now();
        for (int i = 0; i < 10; ++i) {
            state.compositionText = "中文预编辑";
            auto& display = Model::displayState(state, true);
            Model::ensureLayoutCache(display, "monospace", 16.f, 480.f, true);
            if (display.cachedLines.front().metrics.caretX.data() != prefixMetrics) return 9;
            state.compositionText.clear();
            Model::displayState(state, false);
            layout();
            if (state.cachedLines.front().metrics.caretX.data() != prefixMetrics) return 10;
        }
        std::cout << count << " lines: " << std::chrono::duration<double, std::milli>(Clock::now() - compositionStart).count() / 20.0
                  << " ms/preedit update (start/cancel + layout)" << std::endl;
    }
}
