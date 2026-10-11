// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

// Look follows the Ryujinx Harbor (MIT-licensed Ryujinx base), FirstRunView.axaml and HarborIcon.axaml.

#include "harbor_setup/ui.h"
#include "harbor_setup/view.h"

namespace HarborSetup {

NVGcolor Hex(unsigned rgb) {
    return nvgRGB((rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff);
}

NVGcolor WithAlpha(NVGcolor colour, float alpha) {
    colour.a *= alpha;
    return colour;
}

void Polygon(NVGcontext* vg, std::initializer_list<float> points) {
    const float* p = points.begin();
    nvgMoveTo(vg, p[0], p[1]);
    for (size_t i = 2; i < points.size(); i += 2) {
        nvgLineTo(vg, p[i], p[i + 1]);
    }
    nvgClosePath(vg);
}

void Wave(NVGcontext* vg) {
    // M130,377 Q161,354 192,377 T254,377 T316,377 T378,377 L378,393 Q347,416 316,393 T254,393 ... Z
    nvgMoveTo(vg, 130, 377);
    nvgQuadTo(vg, 161, 354, 192, 377);
    nvgQuadTo(vg, 223, 400, 254, 377);
    nvgQuadTo(vg, 285, 354, 316, 377);
    nvgQuadTo(vg, 347, 400, 378, 377);
    nvgLineTo(vg, 378, 393);
    nvgQuadTo(vg, 347, 416, 316, 393);
    nvgQuadTo(vg, 285, 370, 254, 393);
    nvgQuadTo(vg, 223, 416, 192, 393);
    nvgQuadTo(vg, 161, 370, 130, 393);
    nvgClosePath(vg);
}

void ControllerDots(NVGcontext* vg) {
    nvgCircle(vg, 154, 208, 17);
    nvgCircle(vg, 358, 286, 17);
    for (const auto& [cx, cy] : {std::pair{154, 260}, {154, 290}, {139, 275}, {169, 275}, {358, 188},
                                 {358, 218}, {343, 203}, {373, 203}}) {
        nvgCircle(vg, static_cast<float>(cx), static_cast<float>(cy), 6);
    }
}

// The monochrome header emblem: a lighthouse between two controllers (docs/harbor/harbor-icon.svg).
void PaintEmblem(NVGcontext* vg, float x, float y, float w, float h) {
    const auto& p = CurrentPalette();
    nvgTranslate(vg, x, y);
    nvgScale(vg, w / 512.0f, h / 512.0f);
    nvgBeginPath(vg);
    nvgRoundedRectVarying(vg, 106, 142, 90, 212, 52, 0, 0, 52);
    nvgRoundedRectVarying(vg, 316, 142, 90, 212, 0, 52, 52, 0);
    Polygon(vg, {228, 153, 256, 119, 284, 153});
    nvgRect(vg, 235, 164, 42, 34);
    nvgRoundedRect(vg, 228, 206, 56, 12, 6);
    Polygon(vg, {239, 229, 273, 229, 294, 355, 218, 355});
    Wave(vg);
    nvgFillColor(vg, p.ink);
    nvgFill(vg);

    nvgBeginPath(vg);
    ControllerDots(vg);
    Polygon(vg, {235, 253, 277, 253, 280, 271, 232, 271});
    Polygon(vg, {227, 304, 285, 304, 288, 322, 224, 322});
    nvgFillColor(vg, p.paper);
    nvgFill(vg);
}

// Harbor's colour mark: the emblem on its navy tile (Welcome, the boot page).
void PaintMark(NVGcontext* vg, float x, float y, float w, float h) {
    nvgTranslate(vg, x, y);
    nvgScale(vg, w / 512.0f, h / 512.0f);
    const auto fill = [&](NVGcolor colour) {
        nvgFillColor(vg, colour);
        nvgFill(vg);
    };

    nvgBeginPath(vg);
    nvgRoundedRect(vg, 0, 0, 512, 512, 112);
    nvgFillPaint(vg, nvgLinearGradient(vg, 0, 0, 512, 512, Hex(0x213b61), Hex(0x09172c)));
    nvgFill(vg);

    nvgBeginPath(vg);
    nvgRoundedRectVarying(vg, 106, 142, 90, 212, 52, 0, 0, 52);
    fill(Hex(0x42decb));
    nvgBeginPath(vg);
    nvgRoundedRectVarying(vg, 316, 142, 90, 212, 0, 52, 52, 0);
    fill(Hex(0xff7b78));
    nvgBeginPath(vg);
    ControllerDots(vg);
    fill(Hex(0x16304b));
    nvgBeginPath(vg);
    Polygon(vg, {228, 153, 256, 119, 284, 153});
    nvgRoundedRect(vg, 228, 206, 56, 12, 6);
    Polygon(vg, {239, 229, 273, 229, 294, 355, 218, 355});
    fill(Hex(0xf4f7ff));
    nvgBeginPath(vg);
    nvgRect(vg, 235, 164, 42, 34);
    fill(Hex(0xffda88));
    nvgBeginPath(vg);
    nvgRect(vg, 249, 164, 14, 34);
    fill(Hex(0xfff4cf));
    nvgBeginPath(vg);
    Polygon(vg, {235, 253, 277, 253, 280, 271, 232, 271});
    Polygon(vg, {227, 304, 285, 304, 288, 322, 224, 322});
    fill(Hex(0x193251));
    nvgBeginPath(vg);
    Wave(vg);
    fill(Hex(0x59decf));
}

// A footer glyph: a disc in the ink colour with the button's letter in the paper colour.
void PaintButtonGlyph(NVGcontext* vg, float x, float y, float w, float h, const std::string& letter,
                      float alpha) {
    const auto& p = CurrentPalette();
    nvgBeginPath(vg);
    nvgCircle(vg, x + w / 2, y + h / 2, w / 2);
    nvgFillColor(vg, WithAlpha(p.ink, alpha));
    nvgFill(vg);
    nvgFontFaceId(vg, brls::Application::getFont(brls::FONT_REGULAR));
    nvgFontSize(vg, 14);
    nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
    nvgFillColor(vg, WithAlpha(p.paper, alpha));
    nvgText(vg, x + w / 2, y + h / 2 + 0.5f, letter.c_str(), nullptr);
}

void PaintLineGlyph(NVGcontext* vg, float x, float y, float w, float h, int glyph, NVGcolor colour) {
    nvgBeginPath(vg);
    const float cx = x + w / 2;
    const float cy = y + h / 2;
    switch (glyph) {
    case PlusGlyph:
        nvgMoveTo(vg, cx, cy - 7);
        nvgLineTo(vg, cx, cy + 7);
        nvgMoveTo(vg, cx - 7, cy);
        nvgLineTo(vg, cx + 7, cy);
        break;
    case DownloadGlyph:
        nvgMoveTo(vg, cx, cy - 7);
        nvgLineTo(vg, cx, cy + 3);
        nvgMoveTo(vg, cx - 5, cy - 1);
        nvgLineTo(vg, cx, cy + 4);
        nvgLineTo(vg, cx + 5, cy - 1);
        nvgMoveTo(vg, cx - 7, cy + 7);
        nvgLineTo(vg, cx + 7, cy + 7);
        break;
    case CrossGlyph:
        nvgMoveTo(vg, cx - 5.5f, cy - 5.5f);
        nvgLineTo(vg, cx + 5.5f, cy + 5.5f);
        nvgMoveTo(vg, cx + 5.5f, cy - 5.5f);
        nvgLineTo(vg, cx - 5.5f, cy + 5.5f);
        break;
    case CheckGlyph:
        nvgMoveTo(vg, cx - 7, cy);
        nvgLineTo(vg, cx - 2, cy + 5);
        nvgLineTo(vg, cx + 7, cy - 6);
        break;
    case ChevronGlyph:
        nvgMoveTo(vg, cx - 3, cy - 7);
        nvgLineTo(vg, cx + 4, cy);
        nvgLineTo(vg, cx - 3, cy + 7);
        break;
    default:
        return;
    }
    nvgStrokeColor(vg, colour);
    nvgStrokeWidth(vg, 2.25f);
    nvgLineCap(vg, NVG_ROUND);
    nvgLineJoin(vg, NVG_ROUND);
    nvgStroke(vg);
}

brls::Label* MakeText(const std::string& text, float size, NVGcolor colour, bool wrap) {
    auto* label = new brls::Label();
    label->setText(text);
    label->setFontSize(size);
    label->setSingleLine(!wrap);
    label->setAutoAnimate(false);
    label->setTextColor(colour);
    if (wrap) {
        label->setLineHeight(1.4f);
    }
    return label;
}

} // namespace HarborSetup
