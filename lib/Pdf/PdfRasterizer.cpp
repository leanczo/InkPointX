#include "PdfRasterizer.h"

#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <utility>
#include <vector>

namespace {
constexpr int RASTER_WIDTH = 480;
constexpr int MAX_RASTER_HEIGHT = 760;
constexpr size_t MAX_FONT_BYTES = 256 * 1024;
constexpr size_t FONT_READ_CACHE_BYTES = 256;
constexpr size_t PDF_TOKEN_SIZE = 1024;
constexpr int MAX_FORM_DEPTH = 6;
constexpr int MAX_GLYPH_DEPTH = 8;
constexpr size_t MIN_PATH_POINTS = 128;
constexpr size_t OWNED_PATH_POINTS = 512;
constexpr size_t MAX_GLYPH_POINTS = 256;
constexpr size_t MAX_GLYPH_CONTOURS = 32;
constexpr size_t MAX_PENDING_FORMS = 4;
constexpr size_t MAX_GRAPHICS_STATE_DEPTH = 8;

struct Point {
  double x = 0;
  double y = 0;
};

struct Matrix {
  double a = 1;
  double b = 0;
  double c = 0;
  double d = 1;
  double e = 0;
  double f = 0;

  Point apply(const Point p) const { return {a * p.x + c * p.y + e, b * p.x + d * p.y + f}; }
};

Matrix multiply(const Matrix& left, const Matrix& right) {
  return {left.a * right.a + left.c * right.b,
          left.b * right.a + left.d * right.b,
          left.a * right.c + left.c * right.d,
          left.b * right.c + left.d * right.d,
          left.a * right.e + left.c * right.f + left.e,
          left.b * right.e + left.d * right.f + left.f};
}

Matrix translation(const double x, const double y) { return {1, 0, 0, 1, x, y}; }

double matrixScale(const Matrix& matrix) {
  const double sx = std::hypot(matrix.a, matrix.b);
  const double sy = std::hypot(matrix.c, matrix.d);
  return (sx + sy) * 0.5;
}

pdfio_dict_t* resolveDict(pdfio_dict_t* dict, const char* key) {
  if (!dict) return nullptr;
  if (pdfio_dict_t* direct = pdfioDictGetDict(dict, key)) return direct;
  if (pdfio_obj_t* object = pdfioDictGetObj(dict, key)) return pdfioObjGetDict(object);
  return nullptr;
}

pdfio_dict_t* inheritedDict(pdfio_obj_t* object, const char* key) {
  for (int depth = 0; object && depth < 32; ++depth) {
    pdfio_dict_t* dict = pdfioObjGetDict(object);
    if (!dict) return nullptr;
    if (pdfio_dict_t* value = resolveDict(dict, key)) return value;
    object = pdfioDictGetObj(dict, "Parent");
  }
  return nullptr;
}

bool inheritedRect(pdfio_obj_t* object, const char* key, pdfio_rect_t& rect) {
  for (int depth = 0; object && depth < 32; ++depth) {
    pdfio_dict_t* dict = pdfioObjGetDict(object);
    if (!dict) return false;
    if (pdfioDictGetRect(dict, key, &rect)) return true;
    object = pdfioDictGetObj(dict, "Parent");
  }
  return false;
}

pdfio_obj_t* resourceObject(pdfio_dict_t* resources, const char* category, const std::string& name) {
  pdfio_dict_t* categoryDict = resolveDict(resources, category);
  return categoryDict && !name.empty() ? pdfioDictGetObj(categoryDict, name.c_str()) : nullptr;
}

bool parseNumber(const std::string& value, double& output) {
  if (value.empty()) return false;
  char* end = nullptr;
  output = strtod(value.c_str(), &end);
  return end && end != value.c_str() && *end == '\0';
}

double operandNumber(const std::vector<std::string>& operands, const size_t index, const double fallback = 0) {
  if (index >= operands.size()) return fallback;
  double value = fallback;
  parseNumber(operands[index], value);
  return value;
}

std::string operandName(const std::vector<std::string>& operands, const size_t index) {
  if (index >= operands.size()) return {};
  const auto& value = operands[index];
  return !value.empty() && value.front() == '/' ? value.substr(1) : std::string{};
}

bool isOperandToken(const char* token) {
  if (!token || !*token) return false;
  if (token[0] == '/' || token[0] == '(' || token[0] == '<' || token[0] == '[' || token[0] == ']') return true;
  double unused = 0;
  return parseNumber(token, unused);
}

struct Contour {
  const Point* points = nullptr;
  size_t count = 0;
  bool closed = false;
};

class Path {
  Point* points = nullptr;
  size_t pointCapacity = 0;
  size_t pointCount = 0;
  std::array<Contour, 32> contours{};
  size_t contourCount = 0;

  struct ContourRange {
    const Contour* data = nullptr;
    size_t size = 0;
    const Contour* begin() const { return data; }
    const Contour* end() const { return data + size; }
  };

  static int curveSteps(const Point p0, const Point p1, const Point p2) {
    const double length = std::hypot(p1.x - p0.x, p1.y - p0.y) + std::hypot(p2.x - p1.x, p2.y - p1.y);
    return std::clamp(static_cast<int>(std::ceil(length * 0.35)), 2, 16);
  }

 public:
  Path(Point* points, const size_t pointCapacity) : points(points), pointCapacity(pointCapacity) {}
  void clear() {
    pointCount = 0;
    contourCount = 0;
  }
  bool empty() const { return pointCount < 2; }
  ContourRange getContours() const { return {contours.data(), contourCount}; }
  Point current() const {
    return pointCount ? points[pointCount - 1] : Point{};
  }
  void moveTo(const Point point) {
    if (!points || pointCount >= pointCapacity || contourCount >= contours.size()) return;
    points[pointCount] = point;
    contours[contourCount++] = {points + pointCount, 1, false};
    ++pointCount;
  }
  void lineTo(const Point point) {
    if (contourCount == 0) return moveTo(point);
    if (!points || pointCount >= pointCapacity) return;
    points[pointCount++] = point;
    ++contours[contourCount - 1].count;
  }
  void quadraticTo(const Point control, const Point end) {
    if (contours.empty()) return moveTo(end);
    const Point start = current();
    const int steps = curveSteps(start, control, end);
    for (int index = 1; index <= steps; ++index) {
      const double t = static_cast<double>(index) / steps;
      const double u = 1.0 - t;
      lineTo({u * u * start.x + 2 * u * t * control.x + t * t * end.x,
              u * u * start.y + 2 * u * t * control.y + t * t * end.y});
    }
  }
  void cubicTo(const Point c1, const Point c2, const Point end) {
    if (contours.empty()) return moveTo(end);
    const Point start = current();
    const double length = std::hypot(c1.x - start.x, c1.y - start.y) + std::hypot(c2.x - c1.x, c2.y - c1.y) +
                          std::hypot(end.x - c2.x, end.y - c2.y);
    const int steps = std::clamp(static_cast<int>(std::ceil(length * 0.3)), 3, 24);
    for (int index = 1; index <= steps; ++index) {
      const double t = static_cast<double>(index) / steps;
      const double u = 1.0 - t;
      lineTo({u * u * u * start.x + 3 * u * u * t * c1.x + 3 * u * t * t * c2.x + t * t * t * end.x,
              u * u * u * start.y + 3 * u * u * t * c1.y + 3 * u * t * t * c2.y + t * t * t * end.y});
    }
  }
  void close() {
    if (contourCount == 0 || contours[contourCount - 1].count == 0) return;
    contours[contourCount - 1].closed = true;
  }
};

class FileFontData {
  mutable HalFile file;
  mutable std::array<uint8_t, FONT_READ_CACHE_BYTES> cache{};
  mutable size_t cacheOffset = std::numeric_limits<size_t>::max();
  mutable size_t cacheLength = 0;
  size_t length = 0;

 public:
  size_t size() const { return length; }
  uint8_t operator[](const size_t index) const {
    if (index >= length || !file.isOpen()) return 0;
    if (index < cacheOffset || index >= cacheOffset + cacheLength) {
      cacheOffset = index - index % cache.size();
      cacheLength = 0;
      if (!file.seek64(cacheOffset)) return 0;
      const int count = file.read(cache.data(), cache.size());
      if (count <= 0) return 0;
      cacheLength = static_cast<size_t>(count);
    }
    return index < cacheOffset + cacheLength ? cache[index - cacheOffset] : 0;
  }
  bool beginWrite(const std::string& path) {
    close();
    length = 0;
    return Storage.openFileForWrite("PDF", path, file);
  }
  bool append(const uint8_t* bytes, const size_t count) {
    if (!bytes || count > MAX_FONT_BYTES - length || file.write(bytes, count) != count) return false;
    length += count;
    return true;
  }
  bool finishWrite(const std::string& path) {
    file.close();
    cacheOffset = std::numeric_limits<size_t>::max();
    cacheLength = 0;
    return length > 0 && Storage.openFileForRead("PDF", path, file);
  }
  void close() {
    if (file.isOpen()) file.close();
    cacheOffset = std::numeric_limits<size_t>::max();
    cacheLength = 0;
  }
};

template <typename Bytes>
uint16_t be16(const Bytes& bytes, const size_t offset) {
  if (offset + 2 > bytes.size()) return 0;
  return static_cast<uint16_t>((bytes[offset] << 8) | bytes[offset + 1]);
}

template <typename Bytes>
int16_t beS16(const Bytes& bytes, const size_t offset) {
  return static_cast<int16_t>(be16(bytes, offset));
}

template <typename Bytes>
uint32_t be32(const Bytes& bytes, const size_t offset) {
  if (offset + 4 > bytes.size()) return 0;
  return (static_cast<uint32_t>(bytes[offset]) << 24) | (static_cast<uint32_t>(bytes[offset + 1]) << 16) |
         (static_cast<uint32_t>(bytes[offset + 2]) << 8) | bytes[offset + 3];
}

struct GlyphPoint {
  float x = 0;
  float y = 0;
  bool onCurve = false;
};

class TrueTypeFace {
  struct Table {
    uint32_t offset = 0;
    uint32_t length = 0;
  };

  Table head;
  Table maxp;
  Table loca;
  Table glyf;
  Table hhea;
  Table hmtx;
  uint16_t numGlyphs = 0;
  uint16_t numHMetrics = 0;
  int16_t locaFormat = 0;

  Table findTable(const char tag[4]) const {
    if (data.size() < 12) return {};
    const size_t tableCount = be16(data, 4);
    for (size_t index = 0; index < tableCount; ++index) {
      const size_t record = 12 + index * 16;
      if (record + 16 > data.size()) break;
      bool matches = true;
      for (size_t byte = 0; byte < 4; ++byte) matches = matches && data[record + byte] == tag[byte];
      if (!matches) continue;
      const Table result{be32(data, record + 8), be32(data, record + 12)};
      if (static_cast<uint64_t>(result.offset) + result.length <= data.size()) return result;
      return {};
    }
    return {};
  }

  uint32_t glyphOffset(const uint16_t glyph) const {
    if (glyph >= numGlyphs) return 0;
    const size_t entry = loca.offset + static_cast<size_t>(glyph) * (locaFormat ? 4 : 2);
    return locaFormat ? be32(data, entry) : static_cast<uint32_t>(be16(data, entry)) * 2;
  }

  bool appendSimpleGlyph(const size_t offset, const int contoursCount, const Matrix& transform, Path& path) const {
    size_t cursor = offset + 10;
    if (contoursCount <= 0 || static_cast<size_t>(contoursCount) > MAX_GLYPH_CONTOURS ||
        cursor + static_cast<size_t>(contoursCount) * 2 + 2 > data.size())
      return false;
    std::array<uint16_t, MAX_GLYPH_CONTOURS> ends{};
    for (int index = 0; index < contoursCount; ++index) {
      ends[index] = be16(data, cursor);
      cursor += 2;
    }
    const size_t pointCount = static_cast<size_t>(ends[contoursCount - 1]) + 1;
    const size_t instructionBytes = be16(data, cursor);
    cursor += 2;
    if (pointCount == 0 || pointCount > MAX_GLYPH_POINTS || cursor + instructionBytes > data.size()) return false;
    cursor += instructionBytes;

    std::array<uint8_t, MAX_GLYPH_POINTS> flags{};
    size_t flagCount = 0;
    while (flagCount < pointCount && cursor < data.size()) {
      const uint8_t flag = data[cursor++];
      flags[flagCount++] = flag;
      if (flag & 0x08) {
        if (cursor >= data.size()) return false;
        const uint8_t repeats = data[cursor++];
        for (uint8_t repeat = 0; repeat < repeats && flagCount < pointCount; ++repeat) flags[flagCount++] = flag;
      }
    }
    if (flagCount != pointCount) return false;

    std::array<GlyphPoint, MAX_GLYPH_POINTS> points{};
    int coordinate = 0;
    for (size_t index = 0; index < pointCount; ++index) {
      const uint8_t flag = flags[index];
      if (flag & 0x02) {
        if (cursor >= data.size()) return false;
        const int delta = data[cursor++];
        coordinate += (flag & 0x10) ? delta : -delta;
      } else if (!(flag & 0x10)) {
        if (cursor + 2 > data.size()) return false;
        coordinate += beS16(data, cursor);
        cursor += 2;
      }
      points[index].x = coordinate;
      points[index].onCurve = (flag & 0x01) != 0;
    }
    coordinate = 0;
    for (size_t index = 0; index < pointCount; ++index) {
      const uint8_t flag = flags[index];
      if (flag & 0x04) {
        if (cursor >= data.size()) return false;
        const int delta = data[cursor++];
        coordinate += (flag & 0x20) ? delta : -delta;
      } else if (!(flag & 0x20)) {
        if (cursor + 2 > data.size()) return false;
        coordinate += beS16(data, cursor);
        cursor += 2;
      }
      points[index].y = coordinate;
    }

    size_t contourStart = 0;
    for (int contourIndex = 0; contourIndex < contoursCount; ++contourIndex) {
      const size_t contourEnd = ends[contourIndex];
      if (contourEnd < contourStart || contourEnd >= points.size()) return false;
      const size_t count = contourEnd - contourStart + 1;
      const auto at = [&](size_t relative) -> const GlyphPoint& { return points[contourStart + relative % count]; };

      Point start;
      size_t index = 0;
      size_t remaining = count;
      if (at(0).onCurve) {
        start = {at(0).x, at(0).y};
        index = 1;
        remaining = count - 1;
      } else if (at(count - 1).onCurve) {
        start = {at(count - 1).x, at(count - 1).y};
        remaining = count - 1;
      } else {
        start = {(at(count - 1).x + at(0).x) * 0.5, (at(count - 1).y + at(0).y) * 0.5};
      }
      path.moveTo(transform.apply(start));
      size_t consumed = 0;
      while (consumed < remaining) {
        const GlyphPoint& point = at(index + consumed);
        if (point.onCurve) {
          path.lineTo(transform.apply({point.x, point.y}));
          ++consumed;
          continue;
        }
        const GlyphPoint& next = at(index + consumed + 1);
        const Point control = transform.apply({point.x, point.y});
        if (next.onCurve) {
          path.quadraticTo(control, transform.apply({next.x, next.y}));
          consumed += 2;
        } else {
          const Point midpoint{(point.x + next.x) * 0.5, (point.y + next.y) * 0.5};
          path.quadraticTo(control, transform.apply(midpoint));
          ++consumed;
        }
      }
      path.close();
      contourStart = contourEnd + 1;
    }
    return true;
  }

  bool appendGlyphInternal(const uint16_t glyph, const Matrix& transform, Path& path, const int depth) const {
    if (depth > MAX_GLYPH_DEPTH || glyph >= numGlyphs) return false;
    const uint32_t begin = glyphOffset(glyph);
    const uint32_t end = glyph == numGlyphs - 1
                             ? (locaFormat ? be32(data, loca.offset + static_cast<size_t>(numGlyphs) * 4)
                                           : static_cast<uint32_t>(be16(data, loca.offset + static_cast<size_t>(numGlyphs) * 2)) * 2)
                             : glyphOffset(glyph + 1);
    if (begin == end) return true;
    const size_t offset = static_cast<size_t>(glyf.offset) + begin;
    if (end < begin || offset + 10 > data.size() || static_cast<uint64_t>(glyf.offset) + end > data.size()) return false;
    const int16_t contourCount = beS16(data, offset);
    if (contourCount >= 0) return appendSimpleGlyph(offset, contourCount, transform, path);

    size_t cursor = offset + 10;
    uint16_t flags = 0;
    do {
      if (cursor + 4 > data.size()) return false;
      flags = be16(data, cursor);
      const uint16_t childGlyph = be16(data, cursor + 2);
      cursor += 4;
      int arg1 = 0;
      int arg2 = 0;
      if (flags & 0x0001) {
        if (cursor + 4 > data.size()) return false;
        arg1 = beS16(data, cursor);
        arg2 = beS16(data, cursor + 2);
        cursor += 4;
      } else {
        if (cursor + 2 > data.size()) return false;
        arg1 = static_cast<int8_t>(data[cursor]);
        arg2 = static_cast<int8_t>(data[cursor + 1]);
        cursor += 2;
      }
      Matrix component;
      if (flags & 0x0002) {
        component.e = arg1;
        component.f = arg2;
      }
      const auto readF2Dot14 = [&]() {
        const double value = static_cast<double>(beS16(data, cursor)) / 16384.0;
        cursor += 2;
        return value;
      };
      if (flags & 0x0008) {
        if (cursor + 2 > data.size()) return false;
        component.a = component.d = readF2Dot14();
      } else if (flags & 0x0040) {
        if (cursor + 4 > data.size()) return false;
        component.a = readF2Dot14();
        component.d = readF2Dot14();
      } else if (flags & 0x0080) {
        if (cursor + 8 > data.size()) return false;
        component.a = readF2Dot14();
        component.b = readF2Dot14();
        component.c = readF2Dot14();
        component.d = readF2Dot14();
      }
      if (!appendGlyphInternal(childGlyph, multiply(transform, component), path, depth + 1)) return false;
    } while (flags & 0x0020);
    return true;
  }

 public:
  pdfio_obj_t* object = nullptr;
  FileFontData data;
  uint16_t unitsPerEm = 1000;
  bool valid = false;

  bool initialize() {
    head = findTable("head");
    maxp = findTable("maxp");
    loca = findTable("loca");
    glyf = findTable("glyf");
    hhea = findTable("hhea");
    hmtx = findTable("hmtx");
    if (!head.offset || !maxp.offset || !loca.offset || !glyf.offset || head.offset + 54 > data.size() ||
        maxp.offset + 6 > data.size()) {
      return false;
    }
    unitsPerEm = be16(data, head.offset + 18);
    locaFormat = beS16(data, head.offset + 50);
    numGlyphs = be16(data, maxp.offset + 4);
    if (hhea.offset && hhea.offset + 36 <= data.size()) numHMetrics = be16(data, hhea.offset + 34);
    valid = unitsPerEm > 0 && numGlyphs > 0 && (locaFormat == 0 || locaFormat == 1);
    return valid;
  }

  bool appendGlyph(const uint16_t glyph, const Matrix& transform, Path& path) const {
    return valid && appendGlyphInternal(glyph, transform, path, 0);
  }

  uint16_t advance(const uint16_t glyph) const {
    if (!hmtx.offset || numHMetrics == 0) return unitsPerEm;
    const size_t metric = hmtx.offset + static_cast<size_t>(std::min<uint16_t>(glyph, numHMetrics - 1)) * 4;
    return metric + 2 <= data.size() ? be16(data, metric) : unitsPerEm;
  }
};

pdfio_obj_t* embeddedTrueTypeObject(pdfio_obj_t* fontObject) {
  pdfio_dict_t* font = fontObject ? pdfioObjGetDict(fontObject) : nullptr;
  if (!font) return nullptr;
  pdfio_obj_t* descriptorObject = pdfioDictGetObj(font, "FontDescriptor");
  if (!descriptorObject) {
    pdfio_array_t* descendants = pdfioDictGetArray(font, "DescendantFonts");
    pdfio_obj_t* descendant = descendants && pdfioArrayGetSize(descendants) > 0 ? pdfioArrayGetObj(descendants, 0) : nullptr;
    pdfio_dict_t* descendantDict = descendant ? pdfioObjGetDict(descendant) : nullptr;
    descriptorObject = descendantDict ? pdfioDictGetObj(descendantDict, "FontDescriptor") : nullptr;
  }
  pdfio_dict_t* descriptor = descriptorObject ? pdfioObjGetDict(descriptorObject) : nullptr;
  return descriptor ? pdfioDictGetObj(descriptor, "FontFile2") : nullptr;
}

bool loadFontData(pdfio_obj_t* fontObject, TrueTypeFace& face, const std::string& scratchPath) {
  pdfio_obj_t* fontFile = embeddedTrueTypeObject(fontObject);
  if (!fontFile) return false;
  pdfio_stream_t* stream = pdfioObjOpenStream(fontFile, true);
  if (!stream) return false;
  if (!face.data.beginWrite(scratchPath)) {
    pdfioStreamClose(stream);
    return false;
  }
  uint8_t buffer[1024];
  bool success = true;
  for (;;) {
    const ssize_t count = pdfioStreamRead(stream, buffer, sizeof(buffer));
    if (count < 0) {
      success = false;
      break;
    }
    if (count == 0) break;
    if (!face.data.append(buffer, static_cast<size_t>(count))) {
      success = false;
      break;
    }
  }
  pdfioStreamClose(stream);
  face.object = fontObject;
  return success && face.data.finishWrite(scratchPath) && face.initialize();
}

struct GraphicsState {
  Matrix ctm;
  Matrix textMatrix;
  Matrix lineMatrix;
  pdfio_obj_t* fontObject = nullptr;
  double fontSize = 12;
  double lineWidth = 1;
  double horizontalScale = 1;
  double textRise = 0;
  double charSpacing = 0;
  double wordSpacing = 0;
  double leading = 0;
  std::array<double, 8> dashPattern{};
  uint8_t dashCount = 0;
  double dashPhase = 0;
  bool fillBlack = true;
  bool strokeBlack = true;
};

class GraphicsStateStack {
  std::array<GraphicsState, MAX_GRAPHICS_STATE_DEPTH> entries{};
  size_t count = 0;

 public:
  size_t size() const { return count; }
  bool empty() const { return count == 0; }
  void push_back(const GraphicsState& state) {
    if (count < entries.size()) entries[count++] = state;
  }
  GraphicsState& back() { return entries[count - 1]; }
  void pop_back() {
    if (count) --count;
  }
};

struct Intersection {
  double x = 0;
  int winding = 0;
};

class ContentRenderer {
  uint8_t* pixels;
  int width;
  int height;
  int rowBytes;
  Matrix pageMatrix;
  Point* pathPoints;
  size_t pathCapacity;
  const std::string& fontScratchPath;
  TrueTypeFace* activeFace = nullptr;
  bool paintGraphics = true;

  struct PendingForm {
    pdfio_obj_t* object = nullptr;
    GraphicsState state;
    pdfio_dict_t* resources = nullptr;
    int depth = 0;
  };
  std::array<PendingForm, MAX_PENDING_FORMS> pendingForms{};
  size_t pendingFormCount = 0;

  void setPixel(const int x, const int y, const bool black) {
    if (static_cast<unsigned>(x) >= static_cast<unsigned>(width) ||
        static_cast<unsigned>(y) >= static_cast<unsigned>(height))
      return;
    uint8_t& byte = pixels[static_cast<size_t>(y) * rowBytes + (x >> 3)];
    const uint8_t mask = static_cast<uint8_t>(0x80u >> (x & 7));
    if (black)
      byte &= static_cast<uint8_t>(~mask);
    else
      byte |= mask;
  }

  void drawDisc(const int cx, const int cy, const int diameter, const bool black) {
    if (diameter <= 1) return setPixel(cx, cy, black);
    const int radius = std::max(1, diameter / 2);
    for (int y = cy - radius; y <= cy + radius; ++y)
      for (int x = cx - radius; x <= cx + radius; ++x)
        if ((x - cx) * (x - cx) + (y - cy) * (y - cy) <= radius * radius + radius) setPixel(x, y, black);
  }

  void drawLine(const Point start, const Point end, const int thickness, const bool black) {
    const double dx = end.x - start.x;
    const double dy = end.y - start.y;
    const int steps = std::max(1, static_cast<int>(std::ceil(std::max(std::fabs(dx), std::fabs(dy)))));
    for (int index = 0; index <= steps; ++index) {
      const double t = static_cast<double>(index) / steps;
      drawDisc(static_cast<int>(std::lround(start.x + dx * t)), static_cast<int>(std::lround(start.y + dy * t)),
               thickness, black);
    }
  }

  void strokePath(const Path& path, const GraphicsState& state) {
    const int thickness = std::clamp(static_cast<int>(std::lround(state.lineWidth * matrixScale(state.ctm))), 1, 12);
    for (const auto& contour : path.getContours()) {
      if (contour.count < 2) continue;
      size_t dashIndex = 0;
      const double scale = matrixScale(state.ctm);
      double dashRemaining = state.dashCount ? std::max(0.1, state.dashPattern[0] * scale) : 0;
      if (state.dashCount) {
        double phase = std::max(0.0, state.dashPhase * scale);
        while (phase >= dashRemaining && dashRemaining > 0) {
          phase -= dashRemaining;
          dashIndex = (dashIndex + 1) % state.dashCount;
          dashRemaining = std::max(0.1, state.dashPattern[dashIndex] * scale);
        }
        dashRemaining -= phase;
      }
      auto strokeSegment = [&](const Point start, const Point end) {
        if (!state.dashCount) return drawLine(start, end, thickness, state.strokeBlack);
        const double dx = end.x - start.x;
        const double dy = end.y - start.y;
        const double length = std::hypot(dx, dy);
        if (length <= 0) return;
        double position = 0;
        while (position < length) {
          const double amount = std::min(dashRemaining, length - position);
          if ((dashIndex & 1u) == 0) {
            const double t0 = position / length;
            const double t1 = (position + amount) / length;
            drawLine({start.x + dx * t0, start.y + dy * t0}, {start.x + dx * t1, start.y + dy * t1}, thickness,
                     state.strokeBlack);
          }
          position += amount;
          dashRemaining -= amount;
          if (dashRemaining <= 0.001) {
            dashIndex = (dashIndex + 1) % state.dashCount;
            dashRemaining = std::max(0.1, state.dashPattern[dashIndex] * scale);
          }
        }
      };
      for (size_t index = 1; index < contour.count; ++index)
        strokeSegment(contour.points[index - 1], contour.points[index]);
      if (contour.closed) strokeSegment(contour.points[contour.count - 1], contour.points[0]);
    }
  }

  void fillPath(const Path& path, const bool evenOdd, const bool black) {
    double minY = height;
    double maxY = 0;
    for (const auto& contour : path.getContours())
      for (size_t index = 0; index < contour.count; ++index) {
        const Point point = contour.points[index];
        minY = std::min(minY, point.y);
        maxY = std::max(maxY, point.y);
      }
    const int firstRow = std::max(0, static_cast<int>(std::floor(minY)));
    const int lastRow = std::min(height - 1, static_cast<int>(std::ceil(maxY)));
    std::array<Intersection, 128> intersections{};
    for (int y = firstRow; y <= lastRow; ++y) {
      size_t intersectionCount = 0;
      const double scanY = y + 0.5;
      for (const auto& contour : path.getContours()) {
        if (contour.count < 2) continue;
        const size_t segments = contour.closed ? contour.count : contour.count - 1;
        for (size_t index = 0; index < segments; ++index) {
          const Point p1 = contour.points[index];
          const Point p2 = contour.points[(index + 1) % contour.count];
          if (p1.y == p2.y) continue;
          const double low = std::min(p1.y, p2.y);
          const double high = std::max(p1.y, p2.y);
          if (scanY < low || scanY >= high) continue;
          const double x = p1.x + (scanY - p1.y) * (p2.x - p1.x) / (p2.y - p1.y);
          if (intersectionCount < intersections.size())
            intersections[intersectionCount++] = {x, p2.y > p1.y ? 1 : -1};
        }
      }
      std::sort(intersections.begin(), intersections.begin() + intersectionCount,
                [](const Intersection& left, const Intersection& right) { return left.x < right.x; });
      if (evenOdd) {
        for (size_t index = 1; index < intersectionCount; index += 2) {
          const int x0 = std::max(0, static_cast<int>(std::ceil(intersections[index - 1].x - 0.5)));
          const int x1 = std::min(width - 1, static_cast<int>(std::floor(intersections[index].x - 0.5)));
          for (int x = x0; x <= x1; ++x) setPixel(x, y, black);
        }
      } else {
        int winding = 0;
        double startX = 0;
        for (size_t index = 0; index < intersectionCount; ++index) {
          const auto intersection = intersections[index];
          const int previous = winding;
          winding += intersection.winding;
          if (previous == 0 && winding != 0) {
            startX = intersection.x;
          } else if (previous != 0 && winding == 0) {
            const int x0 = std::max(0, static_cast<int>(std::ceil(startX - 0.5)));
            const int x1 = std::min(width - 1, static_cast<int>(std::floor(intersection.x - 0.5)));
            for (int x = x0; x <= x1; ++x) setPixel(x, y, black);
          }
        }
      }
    }
  }

  void drawGlyph(GraphicsState& state, TrueTypeFace& face, const uint16_t glyph) {
    const double scale = state.fontSize / face.unitsPerEm;
    Matrix glyphMatrix{scale * state.horizontalScale, 0, 0, scale, 0, state.textRise};
    glyphMatrix = multiply(state.ctm, multiply(state.textMatrix, glyphMatrix));
    Path outline(pathPoints, pathCapacity);
    if (face.appendGlyph(glyph, glyphMatrix, outline)) fillPath(outline, false, state.fillBlack);
    const double advance = static_cast<double>(face.advance(glyph)) / face.unitsPerEm * state.fontSize + state.charSpacing;
    state.textMatrix = multiply(state.textMatrix, translation(advance * state.horizontalScale, 0));
  }

  void drawTextToken(GraphicsState& state, const std::string& token) {
    if (!activeFace || activeFace->object != state.fontObject || token.empty()) return;
    const auto drawBytes = [&](const auto& nextByte, const size_t byteCount) {
      const bool twoByte = byteCount >= 2 && (byteCount % 2 == 0);
      uint16_t glyph = 0;
      for (size_t index = 0; index < byteCount; ++index) {
        const uint8_t byte = nextByte(index);
        if (!twoByte) {
          drawGlyph(state, *activeFace, byte);
        } else if ((index & 1u) == 0) {
          glyph = static_cast<uint16_t>(byte) << 8;
        } else {
          drawGlyph(state, *activeFace, static_cast<uint16_t>(glyph | byte));
        }
      }
    };
    if (token.front() == '<') {
      const auto hexValue = [](const char ch) {
        if (ch >= '0' && ch <= '9') return ch - '0';
        if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
        if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
        return -1;
      };
      size_t digitCount = 0;
      for (size_t index = 1; index < token.size() && token[index] != '>'; ++index) {
        if (hexValue(token[index]) >= 0) ++digitCount;
      }
      const size_t byteCount = digitCount / 2;
      size_t cursor = 1;
      const auto nextByte = [&](size_t) {
        int high = -1;
        while (cursor < token.size() && token[cursor] != '>') {
          const int digit = hexValue(token[cursor++]);
          if (digit < 0) continue;
          if (high < 0)
            high = digit;
          else
            return static_cast<uint8_t>((high << 4) | digit);
        }
        return static_cast<uint8_t>(0);
      };
      drawBytes(nextByte, byteCount);
    } else if (token.front() == '(') {
      const size_t byteCount = token.size() > 2 ? token.size() - 2 : 0;
      drawBytes([&](const size_t index) { return static_cast<uint8_t>(token[index + 1]); }, byteCount);
    }
  }

  void paintAndClear(Path& path, GraphicsState& state, const bool fill, const bool stroke, const bool evenOdd,
                     const bool close) {
    if (close) path.close();
    if (fill) fillPath(path, evenOdd, state.fillBlack);
    if (stroke) strokePath(path, state);
    path.clear();
  }

  void handleOperator(const std::string& op, const std::vector<std::string>& args, GraphicsState& state,
                      GraphicsStateStack& stack, Path& path, pdfio_dict_t* resources, const int depth) {
    const size_t n = args.size();
    if (op == "q") {
      if (stack.size() < MAX_GRAPHICS_STATE_DEPTH) stack.push_back(state);
    } else if (op == "Q") {
      if (!stack.empty()) {
        state = stack.back();
        stack.pop_back();
      }
    } else if (op == "cm" && n >= 6) {
      const Matrix matrix{operandNumber(args, n - 6), operandNumber(args, n - 5), operandNumber(args, n - 4),
                          operandNumber(args, n - 3), operandNumber(args, n - 2), operandNumber(args, n - 1)};
      state.ctm = multiply(state.ctm, matrix);
    } else if (op == "w" && n >= 1) {
      state.lineWidth = operandNumber(args, n - 1, 1);
    } else if (op == "d" && n >= 3) {
      state.dashCount = 0;
      for (size_t index = 0; index + 1 < n && state.dashCount < state.dashPattern.size(); ++index) {
        double value = 0;
        if (parseNumber(args[index], value) && value > 0) state.dashPattern[state.dashCount++] = value;
      }
      state.dashPhase = operandNumber(args, n - 1);
    } else if (paintGraphics && op == "m" && n >= 2) {
      path.moveTo(state.ctm.apply({operandNumber(args, n - 2), operandNumber(args, n - 1)}));
    } else if (paintGraphics && op == "l" && n >= 2) {
      path.lineTo(state.ctm.apply({operandNumber(args, n - 2), operandNumber(args, n - 1)}));
    } else if (paintGraphics && op == "c" && n >= 6) {
      path.cubicTo(state.ctm.apply({operandNumber(args, n - 6), operandNumber(args, n - 5)}),
                   state.ctm.apply({operandNumber(args, n - 4), operandNumber(args, n - 3)}),
                   state.ctm.apply({operandNumber(args, n - 2), operandNumber(args, n - 1)}));
    } else if (paintGraphics && op == "v" && n >= 4) {
      path.cubicTo(path.current(), state.ctm.apply({operandNumber(args, n - 4), operandNumber(args, n - 3)}),
                   state.ctm.apply({operandNumber(args, n - 2), operandNumber(args, n - 1)}));
    } else if (paintGraphics && op == "y" && n >= 4) {
      const Point end = state.ctm.apply({operandNumber(args, n - 2), operandNumber(args, n - 1)});
      path.cubicTo(state.ctm.apply({operandNumber(args, n - 4), operandNumber(args, n - 3)}), end, end);
    } else if (paintGraphics && op == "re" && n >= 4) {
      const double x = operandNumber(args, n - 4);
      const double y = operandNumber(args, n - 3);
      const double w = operandNumber(args, n - 2);
      const double h = operandNumber(args, n - 1);
      path.moveTo(state.ctm.apply({x, y}));
      path.lineTo(state.ctm.apply({x + w, y}));
      path.lineTo(state.ctm.apply({x + w, y + h}));
      path.lineTo(state.ctm.apply({x, y + h}));
      path.close();
    } else if (paintGraphics && op == "h") {
      path.close();
    } else if (op == "S") {
      if (paintGraphics) paintAndClear(path, state, false, true, false, false);
    } else if (op == "s") {
      if (paintGraphics) paintAndClear(path, state, false, true, false, true);
    } else if (op == "f" || op == "F") {
      if (paintGraphics) paintAndClear(path, state, true, false, false, false);
    } else if (op == "f*") {
      if (paintGraphics) paintAndClear(path, state, true, false, true, false);
    } else if (op == "B" || op == "B*") {
      if (paintGraphics) paintAndClear(path, state, true, true, op == "B*", false);
    } else if (op == "b" || op == "b*") {
      if (paintGraphics) paintAndClear(path, state, true, true, op == "b*", true);
    } else if (op == "n") {
      path.clear();
    } else if ((op == "g" || op == "G") && n >= 1) {
      const bool black = operandNumber(args, n - 1) < 0.65;
      if (op == "g") state.fillBlack = black;
      else state.strokeBlack = black;
    } else if ((op == "rg" || op == "RG") && n >= 3) {
      const double luminance = 0.299 * operandNumber(args, n - 3) + 0.587 * operandNumber(args, n - 2) +
                               0.114 * operandNumber(args, n - 1);
      if (op == "rg") state.fillBlack = luminance < 0.65;
      else state.strokeBlack = luminance < 0.65;
    } else if (op == "scn" || op == "SCN") {
      bool black = true;
      if (n == 1) black = operandNumber(args, 0) < 0.65;
      else if (n >= 3) black = (operandNumber(args, n - 3) + operandNumber(args, n - 2) + operandNumber(args, n - 1)) / 3 < 0.65;
      if (op == "scn") state.fillBlack = black;
      else state.strokeBlack = black;
    } else if (op == "BT") {
      state.textMatrix = {};
      state.lineMatrix = {};
    } else if (op == "Tf" && n >= 2) {
      state.fontObject = resourceObject(resources, "Font", operandName(args, n - 2));
      state.fontSize = operandNumber(args, n - 1, 12);
    } else if (op == "Tm" && n >= 6) {
      state.textMatrix = {operandNumber(args, n - 6), operandNumber(args, n - 5), operandNumber(args, n - 4),
                          operandNumber(args, n - 3), operandNumber(args, n - 2), operandNumber(args, n - 1)};
      state.lineMatrix = state.textMatrix;
    } else if ((op == "Td" || op == "TD") && n >= 2) {
      const double tx = operandNumber(args, n - 2);
      const double ty = operandNumber(args, n - 1);
      if (op == "TD") state.leading = -ty;
      state.lineMatrix = multiply(state.lineMatrix, translation(tx, ty));
      state.textMatrix = state.lineMatrix;
    } else if (op == "T*") {
      state.lineMatrix = multiply(state.lineMatrix, translation(0, -state.leading));
      state.textMatrix = state.lineMatrix;
    } else if (op == "Tz" && n >= 1) {
      state.horizontalScale = operandNumber(args, n - 1, 100) / 100.0;
    } else if (op == "Ts" && n >= 1) {
      state.textRise = operandNumber(args, n - 1);
    } else if (op == "Tc" && n >= 1) {
      state.charSpacing = operandNumber(args, n - 1);
    } else if (op == "Tw" && n >= 1) {
      state.wordSpacing = operandNumber(args, n - 1);
    } else if (op == "TL" && n >= 1) {
      state.leading = operandNumber(args, n - 1);
    } else if (op == "Tj" && n >= 1) {
      drawTextToken(state, args.back());
    } else if (op == "TJ") {
      for (const auto& arg : args) {
        if (!arg.empty() && (arg.front() == '<' || arg.front() == '(')) {
          drawTextToken(state, arg);
        } else {
          double adjustment = 0;
          if (parseNumber(arg, adjustment))
            state.textMatrix = multiply(state.textMatrix,
                                        translation(-adjustment / 1000.0 * state.fontSize * state.horizontalScale, 0));
        }
      }
    } else if (paintGraphics && op == "Do" && n >= 1 && depth < MAX_FORM_DEPTH) {
      pdfio_obj_t* xobject = resourceObject(resources, "XObject", operandName(args, n - 1));
      const char* subtype = xobject ? pdfioObjGetSubtype(xobject) : nullptr;
      if (subtype && strcmp(subtype, "Form") == 0) {
        GraphicsState child = state;
        pdfio_dict_t* objectDict = pdfioObjGetDict(xobject);
        if (pdfio_array_t* matrix = objectDict ? pdfioDictGetArray(objectDict, "Matrix") : nullptr;
            matrix && pdfioArrayGetSize(matrix) >= 6) {
          const Matrix formMatrix{pdfioArrayGetNumber(matrix, 0), pdfioArrayGetNumber(matrix, 1),
                                  pdfioArrayGetNumber(matrix, 2), pdfioArrayGetNumber(matrix, 3),
                                  pdfioArrayGetNumber(matrix, 4), pdfioArrayGetNumber(matrix, 5)};
          child.ctm = multiply(child.ctm, formMatrix);
        }
        pdfio_dict_t* childResources = objectDict ? resolveDict(objectDict, "Resources") : nullptr;
        if (pendingFormCount < pendingForms.size())
          pendingForms[pendingFormCount++] = {xobject, child, childResources ? childResources : resources, depth + 1};
      }
    }
  }

  void renderStream(pdfio_stream_t* stream, GraphicsState& state, GraphicsStateStack& stack, Path& path,
                    pdfio_dict_t* resources, const int depth, TrueTypeFace* face, const bool graphics) {
    activeFace = face;
    paintGraphics = graphics;
    std::vector<std::string> operands;
    operands.reserve(16);
    char token[PDF_TOKEN_SIZE];
    while (pdfioStreamGetToken(stream, token, sizeof(token))) {
      if (isOperandToken(token)) {
        if (operands.size() < 128) operands.emplace_back(token);
        continue;
      }
      handleOperator(token, operands, state, stack, path, resources, depth);
      operands.clear();
    }
    activeFace = nullptr;
    paintGraphics = true;
  }

  void renderPagePass(pdfio_obj_t* page, GraphicsState state, pdfio_dict_t* resources, TrueTypeFace* face,
                      const bool graphics) {
    GraphicsStateStack stack;
    Path path(pathPoints, pathCapacity);
    const size_t count = pdfioPageGetNumStreams(page);
    for (size_t index = 0; index < count; ++index) {
      pdfio_stream_t* stream = pdfioPageOpenStream(page, index, true);
      if (!stream) continue;
      renderStream(stream, state, stack, path, resources, 0, face, graphics);
      pdfioStreamClose(stream);
    }
  }

  void renderPageStreams(pdfio_obj_t* page, const GraphicsState& state, pdfio_dict_t* resources) {
    renderPagePass(page, state, resources, nullptr, true);
    pdfio_dict_t* fontDict = resolveDict(resources, "Font");
    const size_t count = fontDict ? pdfioDictGetNumPairs(fontDict) : 0;
    for (size_t index = 0; index < count; ++index) {
      const char* name = pdfioDictGetKey(fontDict, index);
      pdfio_obj_t* object = name ? pdfioDictGetObj(fontDict, name) : nullptr;
      TrueTypeFace face;
      if (object && loadFontData(object, face, fontScratchPath)) renderPagePass(page, state, resources, &face, false);
    }
  }

  void renderFormPass(const PendingForm& form, TrueTypeFace* face, const bool graphics) {
    pdfio_stream_t* stream = pdfioObjOpenStream(form.object, true);
    if (!stream) return;
    GraphicsState state = form.state;
    GraphicsStateStack stack;
    Path path(pathPoints, pathCapacity);
    renderStream(stream, state, stack, path, form.resources, form.depth, face, graphics);
    pdfioStreamClose(stream);
  }

  void renderForm(const PendingForm& form) {
    if (!form.object || form.depth > MAX_FORM_DEPTH) return;
    renderFormPass(form, nullptr, true);
    pdfio_dict_t* fontDict = resolveDict(form.resources, "Font");
    const size_t count = fontDict ? pdfioDictGetNumPairs(fontDict) : 0;
    for (size_t index = 0; index < count; ++index) {
      const char* name = pdfioDictGetKey(fontDict, index);
      pdfio_obj_t* object = name ? pdfioDictGetObj(fontDict, name) : nullptr;
      TrueTypeFace face;
      if (object && loadFontData(object, face, fontScratchPath)) renderFormPass(form, &face, false);
    }
  }

 public:
  ContentRenderer(uint8_t* pixels, const int width, const int height, const Matrix& pageMatrix, Point* pathPoints,
                  const size_t pathCapacity, const std::string& fontScratchPath)
      : pixels(pixels),
        width(width),
        height(height),
        rowBytes((width + 7) / 8),
        pageMatrix(pageMatrix),
        pathPoints(pathPoints),
        pathCapacity(pathCapacity),
        fontScratchPath(fontScratchPath) {}

  void render(pdfio_obj_t* page) {
    GraphicsState initial;
    initial.ctm = pageMatrix;
    pdfio_dict_t* resources = inheritedDict(page, "Resources");
    renderPageStreams(page, initial, resources);
    for (size_t index = 0; index < pendingFormCount; ++index) {
      const PendingForm form = pendingForms[index];
      renderForm(form);
    }
  }
};

uint32_t crc32Update(uint32_t crc, const uint8_t* data, const size_t length) {
  for (size_t index = 0; index < length; ++index) {
    crc ^= data[index];
    for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
  }
  return crc;
}

bool writeAll(HalFile& file, const void* data, const size_t length) { return file.write(data, length) == length; }

bool writeBe32(HalFile& file, const uint32_t value) {
  const uint8_t bytes[] = {static_cast<uint8_t>(value >> 24), static_cast<uint8_t>(value >> 16),
                           static_cast<uint8_t>(value >> 8), static_cast<uint8_t>(value)};
  return writeAll(file, bytes, sizeof(bytes));
}

bool writePngChunk(HalFile& file, const char type[4], const uint8_t* data, const size_t length) {
  uint32_t crc = 0xffffffffu;
  crc = crc32Update(crc, reinterpret_cast<const uint8_t*>(type), 4);
  if (length) crc = crc32Update(crc, data, length);
  return writeBe32(file, static_cast<uint32_t>(length)) && writeAll(file, type, 4) &&
         (!length || writeAll(file, data, length)) && writeBe32(file, crc ^ 0xffffffffu);
}

uint32_t adlerUpdate(uint32_t adler, const uint8_t* data, const size_t length) {
  uint32_t a = adler & 0xffffu;
  uint32_t b = adler >> 16;
  for (size_t index = 0; index < length; ++index) {
    a = (a + data[index]) % 65521u;
    b = (b + a) % 65521u;
  }
  return (b << 16) | a;
}

bool writeMonochromePng(const std::string& path, const uint8_t* pixels, const int width, const int height) {
  HalFile file;
  if (!Storage.openFileForWrite("PDF", path, file)) return false;
  const uint8_t signature[] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
  uint8_t ihdr[13]{};
  ihdr[0] = static_cast<uint8_t>(width >> 24);
  ihdr[1] = static_cast<uint8_t>(width >> 16);
  ihdr[2] = static_cast<uint8_t>(width >> 8);
  ihdr[3] = static_cast<uint8_t>(width);
  ihdr[4] = static_cast<uint8_t>(height >> 24);
  ihdr[5] = static_cast<uint8_t>(height >> 16);
  ihdr[6] = static_cast<uint8_t>(height >> 8);
  ihdr[7] = static_cast<uint8_t>(height);
  ihdr[8] = 8;  // PNGdec expects one unpacked grayscale byte per sample
  ihdr[9] = 0;  // grayscale
  bool success = writeAll(file, signature, sizeof(signature)) && writePngChunk(file, "IHDR", ihdr, sizeof(ihdr));

  const int rowBytes = (width + 7) / 8;
  std::array<uint8_t, RASTER_WIDTH + 12> block{};
  uint32_t adler = 1;
  const uint8_t filter = 0;
  for (int row = 0; success && row < height; ++row) {
    size_t cursor = 0;
    if (row == 0) {
      block[cursor++] = 0x78;
      block[cursor++] = 0x01;
    }
    const bool final = row == height - 1;
    block[cursor++] = final ? 0x01 : 0x00;  // byte-aligned stored DEFLATE block
    const uint16_t length = static_cast<uint16_t>(width + 1);
    const uint16_t inverse = static_cast<uint16_t>(~length);
    block[cursor++] = static_cast<uint8_t>(length);
    block[cursor++] = static_cast<uint8_t>(length >> 8);
    block[cursor++] = static_cast<uint8_t>(inverse);
    block[cursor++] = static_cast<uint8_t>(inverse >> 8);
    block[cursor++] = filter;
    const size_t pixelOffset = cursor;
    const uint8_t* source = pixels + static_cast<size_t>(row) * rowBytes;
    for (int x = 0; x < width; ++x) {
      block[cursor++] = (source[x >> 3] & (0x80u >> (x & 7))) ? 0xff : 0x00;
    }
    adler = adlerUpdate(adler, &filter, 1);
    adler = adlerUpdate(adler, block.data() + pixelOffset, width);
    if (final) {
      block[cursor++] = static_cast<uint8_t>(adler >> 24);
      block[cursor++] = static_cast<uint8_t>(adler >> 16);
      block[cursor++] = static_cast<uint8_t>(adler >> 8);
      block[cursor++] = static_cast<uint8_t>(adler);
    }
    success = writePngChunk(file, "IDAT", block.data(), cursor);
  }
  if (success) success = writePngChunk(file, "IEND", nullptr, 0);
  file.close();
  if (!success) Storage.remove(path.c_str());
  return success;
}

bool streamNeedsRasterization(pdfio_stream_t* stream, pdfio_dict_t* resources) {
  std::vector<std::string> operands;
  char token[PDF_TOKEN_SIZE];
  while (pdfioStreamGetToken(stream, token, sizeof(token))) {
    if (isOperandToken(token)) {
      if (operands.size() < 128) operands.emplace_back(token);
      continue;
    }
    const bool paintsPath = !strcmp(token, "S") || !strcmp(token, "s") || !strcmp(token, "f") ||
                            !strcmp(token, "F") || !strcmp(token, "f*") || !strcmp(token, "B") ||
                            !strcmp(token, "B*") || !strcmp(token, "b") || !strcmp(token, "b*");
    if (paintsPath) return true;
    if (!strcmp(token, "Do") && !operands.empty()) {
      pdfio_obj_t* object = resourceObject(resources, "XObject", operandName(operands, operands.size() - 1));
      const char* subtype = object ? pdfioObjGetSubtype(object) : nullptr;
      if (subtype && strcmp(subtype, "Form") == 0) return true;
    }
    operands.clear();
  }
  return false;
}
}  // namespace

bool PdfRasterizer::pageNeedsRasterization(pdfio_obj_t* page) {
  if (!page) return false;
  pdfio_dict_t* resources = inheritedDict(page, "Resources");
  const size_t count = pdfioPageGetNumStreams(page);
  for (size_t index = 0; index < count; ++index) {
    pdfio_stream_t* stream = pdfioPageOpenStream(page, index, true);
    if (!stream) continue;
    const bool needed = streamNeedsRasterization(stream, resources);
    pdfioStreamClose(stream);
    if (needed) return true;
  }
  return false;
}

bool PdfRasterizer::renderPage(pdfio_obj_t* page, const std::string& outputPath, std::string& error) {
  pdfio_rect_t box{};
  if (!inheritedRect(page, "CropBox", box) && !inheritedRect(page, "MediaBox", box)) {
    error = "PDF page has no media box";
    return false;
  }
  const double sourceWidth = std::fabs(box.x2 - box.x1);
  const double sourceHeight = std::fabs(box.y2 - box.y1);
  if (sourceWidth < 1 || sourceHeight < 1) {
    error = "PDF page has invalid dimensions";
    return false;
  }
  int width = RASTER_WIDTH;
  int height = static_cast<int>(std::lround(width * sourceHeight / sourceWidth));
  if (height > MAX_RASTER_HEIGHT) {
    height = MAX_RASTER_HEIGHT;
    width = std::max(1, static_cast<int>(std::lround(height * sourceWidth / sourceHeight)));
  }
  const int rowBytes = (width + 7) / 8;
  const size_t required = static_cast<size_t>(rowBytes) * height;
  std::unique_ptr<uint8_t[]> owned;
  uint8_t* buffer = scratch;
  size_t bufferSize = scratchSize;
  const size_t minimumSize = required + MIN_PATH_POINTS * sizeof(Point) + alignof(Point) - 1;
  if (!buffer || bufferSize < minimumSize) {
    bufferSize = required + OWNED_PATH_POINTS * sizeof(Point) + alignof(Point) - 1;
    owned.reset(new (std::nothrow) uint8_t[bufferSize]);
    buffer = owned.get();
  }
  if (!buffer) {
    error = "Not enough memory to rasterize PDF graphics";
    return false;
  }
  const uintptr_t rawPath = reinterpret_cast<uintptr_t>(buffer + required);
  const uintptr_t alignedPath = (rawPath + alignof(Point) - 1) & ~(static_cast<uintptr_t>(alignof(Point)) - 1);
  const size_t alignmentBytes = static_cast<size_t>(alignedPath - rawPath);
  Point* pathPoints = reinterpret_cast<Point*>(alignedPath);
  const size_t pathCapacity = (bufferSize - required - alignmentBytes) / sizeof(Point);
  memset(buffer, 0xff, required);
  const double scale = std::min(static_cast<double>(width) / sourceWidth, static_cast<double>(height) / sourceHeight);
  const double offsetX = (width - sourceWidth * scale) * 0.5;
  const double offsetY = (height - sourceHeight * scale) * 0.5;
  const Matrix pageMatrix{scale, 0, 0, -scale, offsetX - box.x1 * scale, offsetY + box.y2 * scale};
  const std::string fontScratchPath = outputPath + ".font.tmp";
  ContentRenderer renderer(buffer, width, height, pageMatrix, pathPoints, pathCapacity, fontScratchPath);
  renderer.render(page);
  Storage.remove(fontScratchPath.c_str());
  if (!writeMonochromePng(outputPath, buffer, width, height)) {
    error = "Unable to write rasterized PDF page";
    return false;
  }
  return true;
}
