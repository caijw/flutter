// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/fml/logging.h"
#include "flutter/testing/assertions_skia.h"
#include "gtest/gtest.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "third_party/skia/include/core/SkCanvas.h"
#include "third_party/skia/include/core/SkColor.h"
#include "third_party/skia/include/core/SkFont.h"
#include "third_party/skia/include/core/SkFontMgr.h"
#include "third_party/skia/include/core/SkPaint.h"
#include "third_party/skia/include/core/SkPath.h"
#include "third_party/skia/include/core/SkRect.h"
#include "third_party/skia/include/core/SkSurface.h"
#include "third_party/skia/include/core/SkTypeface.h"
#include "third_party/skia/include/core/SkStream.h"
#include "third_party/skia/include/core/SkFontScanner.h"
#include "txt/platform.h"  // txt::GetDefaultFontManager()
#include "third_party/skia/include/core/SkTypeface.h"
#include "third_party/skia/include/core/SkData.h"

namespace flutter {
namespace testing {


static constexpr SkFontTableTag kCmapTag = SkSetFourByteTag('c','m','a','p');

/// Unicode 覆盖范围（闭区间 [start, end]）
struct UnicodeRange {
    int start;
    int end;
};

/// 可变字体设计轴（对应 OpenType fvar 表中的一条轴）
struct VariationAxis {
    uint32_t tag;  // 四字节标签，如 'wght'=0x77676874
    float    min;
    float    def;
    float    max;
};

/// 系统字体元数据（轻量，不含字体文件数据）
struct SystemFontMeta {
    std::string              familyName;
    std::string              assetPath;      // 字体文件路径（由 SkTypeface::getResourceName 获取）
    std::vector<UnicodeRange> coverage;      // Unicode 覆盖范围列表
    size_t                   fileSize;
    int                      weight;         // CSS font-weight: 100-900
    int                      width;          // CSS font-stretch: 1-9
    int                      slant;          // 0=Upright, 1=Italic, 2=Oblique
    std::vector<VariationAxis> variationAxes; // 非空表示可变字体
};

/// 按需加载字体文件的结果
struct LoadFontResult {
    std::string          key;    // "familyName|weight|width|slant"
    std::string          family;
    int                  weight;
    std::vector<uint8_t> data;   // 字体文件二进制数据

    bool isEmpty() const { return data.empty(); }
};

// // 从 SkSurface 读取指定像素点的颜色
// static SkColor ReadPixel(SkSurface* surface, int x, int y) {
//   SkBitmap bitmap;
//   bitmap.allocN32Pixels(1, 1);
//   surface->readPixels(bitmap, x, y);
//   return bitmap.getColor(0, 0);
// }

// ── SkFontMgr 四种获取方式 ────────────────────────────────────────────────────

// 方式一：txt::GetDefaultFontManager()  ← Flutter engine 测试推荐
// 跨平台封装，macOS 底层是 SkFontMgr_New_CoreText(nullptr)


/// 扫描 Unicode 覆盖范围
///
/// 使用 SkTypeface::unicharsToGlyphs() 批量探测。
/// 对 BMP (U+0000~U+FFFF) 按 256 字符为一块批量探测，构建连续覆盖范围列表。
std::vector<UnicodeRange> ScanUnicodeCoverage(const sk_sp<SkTypeface>& typeface) {
   std::vector<UnicodeRange> ranges;

    // 辅助函数：扫描 [scanStart, scanEnd] 区间，将结果追加到 ranges
    // rangeStart 在多次调用间保持状态，支持跨区段合并连续范围
    auto scanRange = [&](int scanStart, int scanEnd, int& rangeStart) {
        constexpr int kBlockSize = 256;
        SkUnichar chars[kBlockSize];
        SkGlyphID glyphs[kBlockSize];

        for (int blockStart = scanStart; blockStart <= scanEnd; blockStart += kBlockSize) {
            int blockEnd = std::min(blockStart + kBlockSize - 1, scanEnd);
            int count = blockEnd - blockStart + 1;

            for (int k = 0; k < count; k++) {
                chars[k] = blockStart + k;
            }
            typeface->unicharsToGlyphs(chars, count, glyphs);

            for (int k = 0; k < count; k++) {
                int cp = blockStart + k;
                // glyph 0 = .notdef，表示字体不支持该字符
                bool covered = (glyphs[k] != 0);
                if (covered) {
                    if (rangeStart < 0) rangeStart = cp;
                } else {
                    if (rangeStart >= 0) {
                        ranges.push_back({rangeStart, cp - 1});
                        rangeStart = -1;
                    }
                }
            }
        }
    };

    int rangeStart = -1;

    // ── 阶段 1：BMP 全量扫描 (U+0000 ~ U+FFFF) ──────────────────────────────
    //
    // 跳过 U+0000~U+001F（C0 控制字符）和 U+007F（DEL）：
    // 部分字体对这些码点返回非零 glyph（映射到 .notdef 或空白字形），
    // 但它们不是可见字符，不应计入覆盖范围。
    // 从 U+0020（空格）开始扫描。
    scanRange(0x0020, 0xFFFF, rangeStart);
    if (rangeStart >= 0) {
        ranges.push_back({rangeStart, 0xFFFF});
        rangeStart = -1;
    }

    // ── 阶段 2：SMP 常用区段扫描 ─────────────────────────────────────────────
    //
    // 只扫描实际有字体内容的区段，跳过大量空白的私有区和保留区。
    // 各区段之间不连续，每段独立处理（rangeStart 在段间重置）。
    struct SmpSegment { int start; int end; const char* name; };
    static const SmpSegment kSmpSegments[] = {
        // 平面 1：多文种补充平面 (SMP)
        {0x10000, 0x1007F, "Linear B Syllabary"},
        {0x10080, 0x100FF, "Linear B Ideograms"},
        {0x10300, 0x1032F, "Old Italic"},
        {0x10330, 0x1034F, "Gothic"},
        {0x10400, 0x1044F, "Deseret"},
        {0x10450, 0x1047F, "Shavian"},
        {0x10480, 0x104AF, "Osmanya"},
        {0x10800, 0x1083F, "Cypriot Syllabary"},
        {0x10900, 0x1091F, "Phoenician"},
        {0x10A00, 0x10A5F, "Kharoshthi"},
        {0x12000, 0x123FF, "Cuneiform"},
        {0x1D000, 0x1D0FF, "Byzantine Musical Symbols"},
        {0x1D100, 0x1D1FF, "Musical Symbols"},
        {0x1D300, 0x1D35F, "Tai Xuan Jing Symbols"},
        {0x1D400, 0x1D7FF, "Mathematical Alphanumeric Symbols"},
        // Emoji（跨多个区段）
        {0x1F000, 0x1F02F, "Mahjong Tiles"},
        {0x1F030, 0x1F09F, "Domino Tiles"},
        {0x1F0A0, 0x1F0FF, "Playing Cards"},
        {0x1F100, 0x1F1FF, "Enclosed Alphanumeric Supplement"},
        {0x1F200, 0x1F2FF, "Enclosed Ideographic Supplement"},
        {0x1F300, 0x1F5FF, "Miscellaneous Symbols and Pictographs"},
        {0x1F600, 0x1F64F, "Emoticons"},
        {0x1F650, 0x1F67F, "Ornamental Dingbats"},
        {0x1F680, 0x1F6FF, "Transport and Map Symbols"},
        {0x1F700, 0x1F77F, "Alchemical Symbols"},
        {0x1F780, 0x1F7FF, "Geometric Shapes Extended"},
        {0x1F800, 0x1F8FF, "Supplemental Arrows-C"},
        {0x1F900, 0x1F9FF, "Supplemental Symbols and Pictographs"},
        {0x1FA00, 0x1FA6F, "Chess Symbols"},
        {0x1FA70, 0x1FAFF, "Symbols and Pictographs Extended-A"},
        // 平面 2：CJK 扩展
        {0x20000, 0x2A6DF, "CJK Unified Ideographs Extension B"},
        {0x2A700, 0x2B73F, "CJK Unified Ideographs Extension C"},
        {0x2B740, 0x2B81F, "CJK Unified Ideographs Extension D"},
        {0x2B820, 0x2CEAF, "CJK Unified Ideographs Extension E"},
        {0x2CEB0, 0x2EBEF, "CJK Unified Ideographs Extension F"},
        {0x2F800, 0x2FA1F, "CJK Compatibility Ideographs Supplement"},
        // 平面 3：CJK 扩展 G
        {0x30000, 0x3134F, "CJK Unified Ideographs Extension G"},
    };

    for (const auto& seg : kSmpSegments) {
        rangeStart = -1;  // 每个区段独立，不跨段合并
        scanRange(seg.start, seg.end, rangeStart);
        if (rangeStart >= 0) {
            ranges.push_back({rangeStart, seg.end});
            rangeStart = -1;
        }
    }

    return ranges;
}

std::vector<UnicodeRange> GetCmapCoverage(
        const sk_sp<SkTypeface>& typeface) {
    std::vector<UnicodeRange> ranges;

    sk_sp<SkData> cmapData = typeface->copyTableData(kCmapTag);
    if (!cmapData || cmapData->size() < 4) return ranges;

    const uint8_t* data = cmapData->bytes();
    size_t size = cmapData->size();

    // cmap 表头：version(2) + numTables(2)
    uint16_t numTables = (data[2] << 8) | data[3];

    // 找 format 4（BMP）和 format 12（全 Unicode）
    const uint8_t* fmt4 = nullptr;
    const uint8_t* fmt12 = nullptr;

    for (uint16_t i = 0; i < numTables; i++) {
        size_t off = 4 + i * 8;
        if (off + 8 > size) break;
        uint32_t subtableOffset = (data[off+4] << 24) | (data[off+5] << 16) |
                                 (data[off+6] << 8) | data[off+7];

        if (subtableOffset + 2 > size) continue;
        uint16_t format = (data[subtableOffset] << 8) | data[subtableOffset+1];

        // 优先 format 12（Unicode 全量，含 SMP）
        if (format == 12 && !fmt12) fmt12 = data + subtableOffset;
        // format 4 覆盖 BMP
        if (format == 4 && !fmt4) fmt4 = data + subtableOffset;
    }

    // 解析 format 12 (SequentialMapGroup)
    if (fmt12 && fmt12 + 16 <= data + size) {
        // format(2) + reserved(2) + length(4) + language(4) + numGroups(4)
        uint32_t numGroups = (fmt12[12] << 24) | (fmt12[13] << 16) |
                             (fmt12[14] << 8) | fmt12[15];
        for (uint32_t g = 0; g < numGroups; g++) {
            const uint8_t* p = fmt12 + 16 + g * 12;
            if (p + 12 > data + size) break;
            uint32_t startChar = (p[0]<<24)|(p[1]<<16)|(p[2]<<8)|p[3];
            uint32_t endChar   = (p[4]<<24)|(p[5]<<16)|(p[6]<<8)|p[7];
            uint32_t startGlyph= (p[8]<<24)|(p[9]<<16)|(p[10]<<8)|p[11];
            if (startGlyph != 0) {  // glyph 0 = .notdef
                ranges.push_back({static_cast<int>(startChar), static_cast<int>(endChar)});
            }
        }
        // 合并连续 range
        std::vector<UnicodeRange> merged;
        for (auto& r : ranges) {
            if (!merged.empty() && merged.back().end + 1 == r.start) {
                merged.back().end = r.end;
            } else {
                merged.push_back(r);
            }
        }
        return merged;  // format 12 已覆盖全部，无需 format 4
    }

    // 解析 format 4 (Segment mapping to delta values)
    if (fmt4 && fmt4 + 14 <= data + size) {
        uint16_t segCountX2 = (fmt4[6] << 8) | fmt4[7];
        uint16_t segCount = segCountX2 / 2;
        const uint8_t* endCodes   = fmt4 + 14;
        const uint8_t* startCodes = endCodes + segCountX2 + 2; // +2 for reservedPad

        for (uint16_t s = 0; s < segCount; s++) {
            uint16_t endCode   = (endCodes[s*2] << 8) | endCodes[s*2+1];
            uint16_t startCode = (startCodes[s*2] << 8) | startCodes[s*2+1];
            if (startCode != 0xFFFF && endCode >= startCode) {
                ranges.push_back({startCode, endCode});
            }
        }
    }

    // 合并连续 range
    std::vector<UnicodeRange> merged;
    for (auto& r : ranges) {
        if (!merged.empty() && merged.back().end + 1 == r.start) {
            merged.back().end = r.end;
        } else {
            merged.push_back(r);
        }
    }
    return merged;
}

/// 打印 SystemFontMeta
void PrintFontMeta(const SystemFontMeta& meta) {
    std::string coverageStr;
    for (size_t c = 0; c < meta.coverage.size(); c++) {
        if (c > 0) coverageStr += ",";
        char buf[32];
        snprintf(buf, sizeof(buf), "%04X-%04X", meta.coverage[c].start, meta.coverage[c].end);
        coverageStr += buf;
    }
    FML_LOG(ERROR) << "Font: " << meta.familyName
                   << " | weight=" << meta.weight
                   << " | width=" << meta.width
                   << " | slant=" << meta.slant
                   << " | size=" << meta.fileSize
                   << " | axes=" << meta.variationAxes.size();
                //    << " | coverage=[" << coverageStr << "]";
}

std::vector<SystemFontMeta> GetSystemFontIndex() {
    sk_sp<SkFontMgr> fontMgr = txt::GetDefaultFontManager();
    std::vector<SystemFontMeta> index;

    int familyCount = fontMgr->countFamilies();
    for (int i = 0; i < familyCount; i++) {
        SkString familyName;
        fontMgr->getFamilyName(i, &familyName);

        sk_sp<SkFontStyleSet> styleSet = fontMgr->createStyleSet(i);
        for (int j = 0; j < styleSet->count(); j++) {
            sk_sp<SkTypeface> typeface = styleSet->createTypeface(j);
            if (!typeface) continue;

            // ① 字体文件路径
            SkString resourceName;
            typeface->getResourceName(&resourceName);
            if (resourceName.isEmpty()) continue;

            // ② 字重 / 字宽 / 倾斜
            SkFontStyle style = typeface->fontStyle();

            // ③ 可变字体轴（先查数量，再读数据；非可变字体返回 0）
            std::vector<VariationAxis> variationAxes;
            int axisCount = typeface->getVariationDesignParameters(nullptr, 0);
            if (axisCount > 0) {
                std::vector<SkFontParameters::Variation::Axis> axes(axisCount);
                typeface->getVariationDesignParameters(axes.data(), axisCount);
                for (const auto& axis : axes) {
                    variationAxes.push_back({
                        .tag = axis.tag,
                        .min = axis.min,
                        .def = axis.def,
                        .max = axis.max,
                    });
                }
            }

            // ④ 文件大小（openExistingStream 不读内容，只获取流长度）
            int ttcIndex = 0;
            auto stream = typeface->openExistingStream(&ttcIndex);
            size_t fileSize = stream ? stream->getLength() : 0;

            // ⑤ Unicode 覆盖范围
            std::vector<UnicodeRange> coverage = ScanUnicodeCoverage(typeface);

            index.push_back(SystemFontMeta{
                .familyName    = familyName.c_str(),
                .assetPath     = resourceName.c_str(),
                .coverage      = std::move(coverage),
                .fileSize      = fileSize,
                .weight        = style.weight(),
                .width         = style.width(),
                .slant         = static_cast<int>(style.slant()),
                .variationAxes = std::move(variationAxes),
            });

            PrintFontMeta(index.back());
        }
    }
    return index;
}



///
/// 使用 SkTypeface::getVariationDesignParameters()
///
/// 使用 SkTypeface::getVariationDesignParameters()
std::vector<VariationAxis> ScanVariationAxes(const sk_sp<SkTypeface>& typeface) {
    int axisCount = typeface->getVariationDesignParameters(nullptr, 0);
    if (axisCount <= 0) return {};

    std::vector<SkFontParameters::Variation::Axis> axes(axisCount);
    typeface->getVariationDesignParameters(axes.data(), axisCount);

    std::vector<VariationAxis> result;
    for (const auto& axis : axes) {
        result.push_back({.tag = axis.tag, .min = axis.min, .def = axis.def, .max = axis.max});
    }
    return result;
}

TEST(SkiaApiTest, GetDefaultFontManager) {
  sk_sp<SkFontMgr> font_mgr = txt::GetDefaultFontManager();

  std::vector<SystemFontMeta> index;

  int familyCount = font_mgr->countFamilies();

  FML_LOG(ERROR) << "字体族数量: " << familyCount;

    for (int i = 0; i < familyCount; i++) {
        SkString familyName;
        font_mgr->getFamilyName(i, &familyName);
                sk_sp<SkFontStyleSet> styleSet = font_mgr->createStyleSet(i);
                int styleCount = styleSet->count();
        FML_LOG(ERROR) << "familyName=" << familyName.c_str() << " styleCount=" << styleCount;
        for (int j = 0; j < styleCount; j++) {
            sk_sp<SkTypeface> typeface = styleSet->createTypeface(j);
            if (!typeface) continue;

            SkString resourceName;
            typeface->getResourceName(&resourceName);
            FML_LOG(ERROR) << "styleCount j=" << j << " resourceName=" << resourceName.c_str();

            // if (resourceName.isEmpty()) continue;
            // ② 字重/字宽/倾斜：SkTypeface::fontStyle()
            SkFontStyle style = typeface->fontStyle();
           // ③ Unicode 覆盖：SkTypeface::unicharsToGlyphs()
            std::vector<UnicodeRange> coverage = ScanUnicodeCoverage(typeface);
            // ④ 可变字体轴：SkTypeface::getVariationDesignParameters()
            std::vector<VariationAxis> variationAxes = ScanVariationAxes(typeface);
            // ⑤ 文件大小：SkTypeface::openExistingStream()
            int ttcIndex = 0;
            auto stream = typeface->openExistingStream(&ttcIndex);
            size_t fileSize = stream ? stream->getLength() : 0;

            index.push_back(SystemFontMeta{
                .familyName    = familyName.c_str(),
                .assetPath     = resourceName.c_str(),
                .coverage      = std::move(coverage),
                .fileSize      = fileSize,
                .weight        = style.weight(),
                .width         = style.width(),
                .slant         = static_cast<int>(style.slant()),
                .variationAxes = std::move(variationAxes),
            });

            PrintFontMeta(index.back());
        }
    }


  ASSERT_NE(font_mgr, nullptr);
  EXPECT_GT(font_mgr->countFamilies(), 0);
}

// // 方式二：SkFontMgr::RefEmpty()
// // 空 FontMgr，不含任何字体，适合隔离测试
// // 注：此版本 Skia 未暴露 RefDefault() / FromData()，跨平台默认 mgr 请用 txt::GetDefaultFontManager()
// TEST(SkiaFontMgrTest, RefEmpty) {
//   sk_sp<SkFontMgr> font_mgr = SkFontMgr::RefEmpty();
//   ASSERT_NE(font_mgr, nullptr);
//   EXPECT_EQ(font_mgr->countFamilies(), 0);
// }

// // ── 用 FontMgr 匹配字体族 ─────────────────────────────────────────────────────

// TEST(SkiaFontMgrTest, MatchFamilyStyle) {
//   sk_sp<SkFontMgr> font_mgr = txt::GetDefaultFontManager();
//   ASSERT_NE(font_mgr, nullptr);

//   // nullptr 表示匹配系统默认字体族
//   sk_sp<SkTypeface> typeface =
//       font_mgr->matchFamilyStyle(nullptr, SkFontStyle::Normal());
//   EXPECT_NE(typeface, nullptr);
// }

// // ── 用 FontMgr 创建 SkFont 并绘制文字 ────────────────────────────────────────

// TEST(SkiaFontMgrTest, DrawTextWithFontMgr) {
//   sk_sp<SkFontMgr> font_mgr = txt::GetDefaultFontManager();
//   ASSERT_NE(font_mgr, nullptr);

//   sk_sp<SkTypeface> typeface =
//       font_mgr->matchFamilyStyle(nullptr, SkFontStyle::Normal());
//   ASSERT_NE(typeface, nullptr);

//   auto surface = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(200, 50));
//   ASSERT_NE(surface, nullptr);
//   SkCanvas* canvas = surface->getCanvas();
//   canvas->clear(SK_ColorWHITE);

//   SkFont font(typeface, 16.0f);
//   SkPaint paint;
//   paint.setColor(SK_ColorBLACK);
//   canvas->drawSimpleText("Hello", 5, SkTextEncoding::kUTF8, 10, 30, font,
//                          paint);

//   EXPECT_NE(surface->makeImageSnapshot(), nullptr);
// }

// // ── Surface 基础测试 ──────────────────────────────────────────────────────────

// TEST(SkiaApiTest, CreateRasterSurface) {
//   auto surface = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(100, 100));
//   ASSERT_NE(surface, nullptr);
//   EXPECT_EQ(surface->width(), 100);
//   EXPECT_EQ(surface->height(), 100);
// }

// TEST(SkiaApiTest, DrawFilledRect) {
//   auto surface = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(100, 100));
//   SkCanvas* canvas = surface->getCanvas();
//   canvas->clear(SK_ColorWHITE);

//   SkPaint paint;
//   paint.setColor(SK_ColorRED);
//   paint.setStyle(SkPaint::kFill_Style);
//   canvas->drawRect(SkRect::MakeLTRB(10, 10, 50, 50), paint);

//   EXPECT_EQ(ReadPixel(surface.get(), 30, 30), SK_ColorRED);
//   EXPECT_EQ(ReadPixel(surface.get(), 5, 5), SK_ColorWHITE);
// }

}  // namespace testing
}  // namespace flutter
