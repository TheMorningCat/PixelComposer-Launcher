// Modified from EUI-NEO by PXCLauncher (2026-11). See MODIFICATIONS.md in this directory.

#pragma once

#include "core/render/render_types.h"
#include "core/render/text_types.h"

#include <memory>
#include <cstdint>
#include <string>
#include <vector>

namespace core {

// ---- 全局文本缩放（docs/06 §10.4.2，T2 方案）----
// 乘在「字号 + 行高」上，**不乘 size()**：控件框是布局常数，把框也乘上去就等于第二个 uiScale，
// 两套倍率语义会撞车（用户不知道该调哪个）。
// 读取点在 `core/dsl.h` 的 `TextBuilder::fontSize/lineHeight`，因此改这个值**必须让调用方重排**
// （`app::setTextScale()` 会顺带触发一次 recompose）；只改值不重排，已建好的元素仍用旧字号。
// 值必须是离散档：字形图集是全局单页且无淘汰，字号一变就多一批新字形，连续变化会把图集撑爆
// 并让文字永久消失（§10.4.2.1 的五条证据）。
void setGlobalTextScale(float scale);
float globalTextScale();

class TextPrimitive {
public:
    struct Glyph {
        float advance = 0.0f;
        float xOffset = 0.0f;
        float yOffset = 0.0f;
        float width = 0.0f;
        float height = 0.0f;
        float u0 = 0.0f;
        float v0 = 0.0f;
        float u1 = 0.0f;
        float v1 = 0.0f;
        bool colored = false;
    };

    struct ShapedGlyph {
        std::uint64_t key = 0;
        unsigned int codepoint = 0;
        int byteStart = 0;
        int byteEnd = 0;
        float advance = 0.0f;
        float xOffset = 0.0f;
        float yOffset = 0.0f;
    };

    struct TextMetrics {
        float width = 0.0f;
        std::vector<int> byteIndices;
        std::vector<float> caretX;
    };

    TextPrimitive();
    TextPrimitive(float x, float y);
    ~TextPrimitive();

    TextPrimitive(const TextPrimitive&) = delete;
    TextPrimitive& operator=(const TextPrimitive&) = delete;
    TextPrimitive(TextPrimitive&&) noexcept;
    TextPrimitive& operator=(TextPrimitive&&) noexcept;

    bool initialize();
    void destroy();

    void setPosition(float x, float y);
    void setText(const std::string& text);
    void setFontFamily(const std::string& fontFamily);
    void setFontSize(float fontSize);
    void setFontWeight(int fontWeight);
    void setColor(const Color& color);
    void setMaxWidth(float maxWidth);
    void setWrap(bool wrap);
    void setHorizontalAlign(HorizontalAlign align);
    void setVerticalAlign(VerticalAlign align);
    void setLineHeight(float lineHeight);
    void setStyle(const TextStyle& style);
    void setVisualScale(float originX, float originY, float scale);
    void setTransform(const Transform& transform, const Rect& frame);
    void setTransformMatrix(const TransformMatrix& matrix);

    const TextStyle& style() const;
    Vec2 position() const;
    Vec2 measuredSize();
    static float measureTextWidth(const std::string& text,
                                  const std::string& fontFamily = {},
                                  float fontSize = 16.0f,
                                  int fontWeight = 400);
    static TextMetrics measureTextMetrics(const std::string& text,
                                          const std::string& fontFamily = {},
                                          float fontSize = 16.0f,
                                          int fontWeight = 400);
    static Vec2 measureTextSize(const TextStyle& style);
    static void setDefaultFontFiles(const std::string& textFontFile, const std::string& iconFontFile);

    void prepare();
    void render(int windowWidth, int windowHeight);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace core
