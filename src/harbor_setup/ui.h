// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

// Drawing pieces shared by the setup wizard and the Harbor menu: colours, the header emblem, the
// footer's button glyphs and line glyphs, all in the 1280x720 design canvas.

#pragma once

#include <functional>
#include <initializer_list>
#include <string>

#include <borealis.hpp>

namespace HarborSetup {

NVGcolor Hex(unsigned rgb);
NVGcolor WithAlpha(NVGcolor colour, float alpha);

// A view that paints itself with nanovg: the marks, verdict discs, step dots, glyphs and bars.
class Shape : public brls::View {
public:
    using Painter = std::function<void(NVGcontext*, float, float, float, float)>;

    Shape(float width, float height, Painter painter_) : painter{std::move(painter_)} {
        setDimensions(width, height);
    }

    void draw(NVGcontext* vg, float x, float y, float width, float height, brls::Style,
              brls::FrameContext*) override {
        nvgSave(vg);
        painter(vg, x, y, width, height);
        nvgRestore(vg);
    }

private:
    Painter painter;
};

void Polygon(NVGcontext* vg, std::initializer_list<float> points);
void Wave(NVGcontext* vg);
void ControllerDots(NVGcontext* vg);

/// The monochrome header emblem in the ink colour.
void PaintEmblem(NVGcontext* vg, float x, float y, float w, float h);

/// A footer glyph: a disc in the ink colour with the button's letter in the paper colour.
void PaintButtonGlyph(NVGcontext* vg, float x, float y, float w, float h, const std::string& letter,
                      float alpha);

enum Glyph { NoGlyph, PlusGlyph, DownloadGlyph, CrossGlyph, CheckGlyph, ChevronGlyph };

void PaintLineGlyph(NVGcontext* vg, float x, float y, float w, float h, int glyph, NVGcolor colour);

/// A single-line (or wrapped) label in the setup's style.
brls::Label* MakeText(const std::string& text, float size, NVGcolor colour, bool wrap = false);

} // namespace HarborSetup
