/*
 * Copyright 2019 Google Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "paragraph_skia.h"

#include <algorithm>
#include <numeric>

namespace txt {

namespace skt = skia::textlayout;
using PaintID = skt::ParagraphPainter::PaintID;

using namespace flutter;

namespace {

// Convert SkFontStyle::Weight values (ranging from 100-900) to txt::FontWeight
// values (ranging from 0-8).
txt::FontWeight GetTxtFontWeight(int font_weight) {
  int txt_weight = (font_weight - 100) / 100;
  txt_weight = std::clamp(txt_weight, static_cast<int>(txt::FontWeight::w100),
                          static_cast<int>(txt::FontWeight::w900));
  return static_cast<txt::FontWeight>(txt_weight);
}

txt::FontStyle GetTxtFontStyle(SkFontStyle::Slant font_slant) {
  return font_slant == SkFontStyle::Slant::kUpright_Slant
             ? txt::FontStyle::normal
             : txt::FontStyle::italic;
}

class DisplayListParagraphPainter : public skt::ParagraphPainter {
 public:
  DisplayListParagraphPainter(DisplayListBuilder* builder,
                              const std::vector<DlPaint>& dl_paints)
      : builder_(builder), dl_paints_(dl_paints) {}

  DlPaint toDlPaint(const DecorationStyle& decor_style,
                    DlDrawStyle draw_style = DlDrawStyle::kStroke) {
    DlPaint paint;
    paint.setDrawStyle(draw_style);
    paint.setAntiAlias(true);
    paint.setColor(decor_style.getColor());
    paint.setStrokeWidth(decor_style.getStrokeWidth());
    std::optional<DashPathEffect> dash_path_effect =
        decor_style.getDashPathEffect();
    if (dash_path_effect) {
      std::array<SkScalar, 2> intervals{dash_path_effect->fOnLength,
                                        dash_path_effect->fOffLength};
      paint.setPathEffect(
          DlDashPathEffect::Make(intervals.data(), intervals.size(), 0));
    }
    return paint;
  }

  void drawTextBlob(const sk_sp<SkTextBlob>& blob,
                    SkScalar x,
                    SkScalar y,
                    const SkPaintOrID& paint) override {
    if (!blob) {
      return;
    }
    size_t paint_id = std::get<PaintID>(paint);
    FML_DCHECK(paint_id < dl_paints_.size());
    builder_->drawTextBlob(blob, x, y, dl_paints_[paint_id]);
  }

  void drawTextShadow(const sk_sp<SkTextBlob>& blob,
                      SkScalar x,
                      SkScalar y,
                      SkColor color,
                      SkScalar blur_sigma) override {
    if (!blob) {
      return;
    }
    DlPaint paint;
    paint.setColor(color);
    if (blur_sigma > 0.0) {
      DlBlurMaskFilter filter(SkBlurStyle::kNormal_SkBlurStyle, blur_sigma,
                              false);
      paint.setMaskFilter(&filter);
    }
    builder_->drawTextBlob(blob, x, y, paint);
  }

  void drawRect(const SkRect& rect, const SkPaintOrID& paint) override {
    size_t paint_id = std::get<PaintID>(paint);
    FML_DCHECK(paint_id < dl_paints_.size());
    builder_->drawRect(rect, dl_paints_[paint_id]);
  }

  void drawFilledRect(const SkRect& rect,
                      const DecorationStyle& decor_style) override {
    DlPaint paint = toDlPaint(decor_style, DlDrawStyle::kFill);
    builder_->drawRect(rect, paint);
  }

  void drawPath(const SkPath& path,
                const DecorationStyle& decor_style) override {
    builder_->drawPath(path, toDlPaint(decor_style));
  }

  void drawLine(SkScalar x0,
                SkScalar y0,
                SkScalar x1,
                SkScalar y1,
                const DecorationStyle& decor_style) override {
    builder_->drawLine(SkPoint::Make(x0, y0), SkPoint::Make(x1, y1),
                       toDlPaint(decor_style));
  }

  void clipRect(const SkRect& rect) override {
    builder_->clipRect(rect, SkClipOp::kIntersect, false);
  }

  void translate(SkScalar dx, SkScalar dy) override {
    builder_->translate(dx, dy);
  }

  void save() override { builder_->save(); }

  void restore() override { builder_->restore(); }

 private:
  DisplayListBuilder* builder_;
  const std::vector<DlPaint>& dl_paints_;
};

}  // anonymous namespace

ParagraphSkia::ParagraphSkia(std::unique_ptr<skt::Paragraph> paragraph,
                             std::vector<flutter::DlPaint>&& dl_paints)
    : paragraph_(std::move(paragraph)), dl_paints_(dl_paints) {}

double ParagraphSkia::GetMaxWidth() {
  return SkScalarToDouble(paragraph_->getMaxWidth());
}

double ParagraphSkia::GetHeight() {
  return SkScalarToDouble(paragraph_->getHeight());
}

double ParagraphSkia::GetLongestLine() {
  return SkScalarToDouble(paragraph_->getLongestLine());
}

std::vector<LineMetrics>& ParagraphSkia::GetLineMetrics() {
  if (!line_metrics_) {
    std::vector<skt::LineMetrics> metrics;
    paragraph_->getLineMetrics(metrics);

    line_metrics_.emplace();
    line_metrics_styles_.reserve(
        std::accumulate(metrics.begin(), metrics.end(), 0,
                        [](const int a, const skt::LineMetrics& b) {
                          return a + b.fLineMetrics.size();
                        }));

    for (const skt::LineMetrics& skm : metrics) {
      LineMetrics& txtm = line_metrics_->emplace_back(
          skm.fStartIndex, skm.fEndIndex, skm.fEndExcludingWhitespaces,
          skm.fEndIncludingNewline, skm.fHardBreak);
      txtm.ascent = skm.fAscent;
      txtm.descent = skm.fDescent;
      txtm.unscaled_ascent = skm.fUnscaledAscent;
      txtm.height = skm.fHeight;
      txtm.width = skm.fWidth;
      txtm.left = skm.fLeft;
      txtm.baseline = skm.fBaseline;
      txtm.line_number = skm.fLineNumber;

      for (const auto& sk_iter : skm.fLineMetrics) {
        const skt::StyleMetrics& sk_style_metrics = sk_iter.second;
        line_metrics_styles_.push_back(SkiaToTxt(*sk_style_metrics.text_style));
        txtm.run_metrics.emplace(
            std::piecewise_construct, std::forward_as_tuple(sk_iter.first),
            std::forward_as_tuple(&line_metrics_styles_.back(),
                                  sk_style_metrics.font_metrics));
      }
    }
  }

  return line_metrics_.value();
}

double ParagraphSkia::GetMinIntrinsicWidth() {
  return SkScalarToDouble(paragraph_->getMinIntrinsicWidth());
}

double ParagraphSkia::GetMaxIntrinsicWidth() {
  return SkScalarToDouble(paragraph_->getMaxIntrinsicWidth());
}

double ParagraphSkia::GetAlphabeticBaseline() {
  return SkScalarToDouble(paragraph_->getAlphabeticBaseline());
}

double ParagraphSkia::GetIdeographicBaseline() {
  return SkScalarToDouble(paragraph_->getIdeographicBaseline());
}

bool ParagraphSkia::DidExceedMaxLines() {
  return paragraph_->didExceedMaxLines();
}

void ParagraphSkia::Layout(double width) {
  line_metrics_.reset();
  line_metrics_styles_.clear();
  paragraph_->layout(width);
}

bool ParagraphSkia::Paint(DisplayListBuilder* builder, double x, double y) {
  DisplayListParagraphPainter painter(builder, dl_paints_);
  paragraph_->paint(&painter, x, y);
  return true;
}

std::vector<Paragraph::TextBox> ParagraphSkia::GetRectsForRange(
    size_t start,
    size_t end,
    RectHeightStyle rect_height_style,
    RectWidthStyle rect_width_style) {
  std::vector<skt::TextBox> skia_boxes = paragraph_->getRectsForRange(
      start, end, static_cast<skt::RectHeightStyle>(rect_height_style),
      static_cast<skt::RectWidthStyle>(rect_width_style));

  std::vector<Paragraph::TextBox> boxes;
  for (const skt::TextBox& skia_box : skia_boxes) {
    boxes.emplace_back(skia_box.rect,
                       static_cast<TextDirection>(skia_box.direction));
  }

  return boxes;
}

std::vector<Paragraph::TextBox> ParagraphSkia::GetRectsForPlaceholders() {
  std::vector<skt::TextBox> skia_boxes = paragraph_->getRectsForPlaceholders();

  std::vector<Paragraph::TextBox> boxes;
  for (const skt::TextBox& skia_box : skia_boxes) {
    boxes.emplace_back(skia_box.rect,
                       static_cast<TextDirection>(skia_box.direction));
  }

  return boxes;
}

Paragraph::PositionWithAffinity ParagraphSkia::GetGlyphPositionAtCoordinate(
    double dx,
    double dy) {
  skt::PositionWithAffinity skia_pos =
      paragraph_->getGlyphPositionAtCoordinate(dx, dy);

  return ParagraphSkia::PositionWithAffinity(
      skia_pos.position, static_cast<Affinity>(skia_pos.affinity));
}

Paragraph::Range<size_t> ParagraphSkia::GetWordBoundary(size_t offset) {
  skt::SkRange<size_t> range = paragraph_->getWordBoundary(offset);
  return Paragraph::Range<size_t>(range.start, range.end);
}

TextStyle ParagraphSkia::SkiaToTxt(const skt::TextStyle& skia) {
  TextStyle txt;

  txt.color = skia.getColor();
  txt.decoration = static_cast<TextDecoration>(skia.getDecorationType());
  txt.decoration_color = skia.getDecorationColor();
  txt.decoration_style =
      static_cast<TextDecorationStyle>(skia.getDecorationStyle());
  txt.decoration_thickness_multiplier =
      SkScalarToDouble(skia.getDecorationThicknessMultiplier());
  txt.font_weight = GetTxtFontWeight(skia.getFontStyle().weight());
  txt.font_style = GetTxtFontStyle(skia.getFontStyle().slant());

  txt.text_baseline = static_cast<TextBaseline>(skia.getTextBaseline());

  for (const SkString& font_family : skia.getFontFamilies()) {
    txt.font_families.emplace_back(font_family.c_str());
  }

  txt.font_size = SkScalarToDouble(skia.getFontSize());
  txt.letter_spacing = SkScalarToDouble(skia.getLetterSpacing());
  txt.word_spacing = SkScalarToDouble(skia.getWordSpacing());
  txt.height = SkScalarToDouble(skia.getHeight());

  txt.locale = skia.getLocale().c_str();
  if (skia.hasBackground()) {
    skt::ParagraphPainter::SkPaintOrID background =
        skia.getBackgroundPaintOrID();
    if (std::holds_alternative<SkPaint>(background)) {
      txt.background = std::get<SkPaint>(background);
    } else if (std::holds_alternative<PaintID>(background)) {
      txt.background_dl = dl_paints_[std::get<PaintID>(background)];
    }
  }
  if (skia.hasForeground()) {
    skt::ParagraphPainter::SkPaintOrID foreground =
        skia.getForegroundPaintOrID();
    if (std::holds_alternative<SkPaint>(foreground)) {
      txt.foreground = std::get<SkPaint>(foreground);
    } else if (std::holds_alternative<PaintID>(foreground)) {
      txt.foreground_dl = dl_paints_[std::get<PaintID>(foreground)];
    }
  }

  txt.text_shadows.clear();
  for (const skt::TextShadow& skia_shadow : skia.getShadows()) {
    txt::TextShadow shadow;
    shadow.offset = skia_shadow.fOffset;
    shadow.blur_sigma = skia_shadow.fBlurSigma;
    shadow.color = skia_shadow.fColor;
    txt.text_shadows.emplace_back(shadow);
  }

  return txt;
}

}  // namespace txt
