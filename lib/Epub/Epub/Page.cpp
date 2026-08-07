#include "Page.h"

#include <GfxRenderer.h>
#include <Logging.h>
#include <Serialization.h>

#include <new>

namespace {

template <typename Predicate>
void renderFilteredPageElements(const std::vector<std::shared_ptr<PageElement>>& elements, GfxRenderer& renderer,
                                const int fontId, const int xOffset, const int yOffset, Predicate&& predicate) {
  for (const auto& element : elements) {
    if (predicate(*element)) {
      element->render(renderer, fontId, xOffset, yOffset);
    }
  }
}

}  // namespace

void PageLine::render(GfxRenderer& renderer, const int fontId, const int xOffset, const int yOffset) {
  block->render(renderer, fontId, xPos + xOffset, yPos + yOffset);
}

bool PageLine::serialize(HalFile& file) {
  serialization::writePod(file, xPos);
  serialization::writePod(file, yPos);

  // serialize TextBlock pointed to by PageLine
  return block->serialize(file);
}

std::unique_ptr<PageLine> PageLine::deserialize(HalFile& file) {
  int16_t xPos = 0;
  int16_t yPos = 0;
  if (!serialization::readPod(file, xPos) || !serialization::readPod(file, yPos)) return nullptr;

  auto tb = TextBlock::deserialize(file);
  if (!tb) return nullptr;
  return std::unique_ptr<PageLine>(new (std::nothrow) PageLine(std::move(tb), xPos, yPos));
}

void PageImage::render(GfxRenderer& renderer, const int fontId, const int xOffset, const int yOffset) {
  // Images don't use fontId or text rendering
  imageBlock->render(renderer, xPos + xOffset, yPos + yOffset);
}

bool PageImage::renderViewport(GfxRenderer& renderer, const int xOffset, const int yOffset, const int sourceX,
                               const int sourceY, const int sourceWidth, const int sourceHeight,
                               const int destinationWidth, const int destinationHeight) {
  return imageBlock->renderViewport(renderer, xPos + xOffset, yPos + yOffset, sourceX, sourceY, sourceWidth,
                                    sourceHeight, destinationWidth, destinationHeight);
}

bool PageImage::serialize(HalFile& file) {
  serialization::writePod(file, xPos);
  serialization::writePod(file, yPos);

  // serialize ImageBlock
  return imageBlock->serialize(file);
}

std::unique_ptr<PageImage> PageImage::deserialize(HalFile& file) {
  int16_t xPos = 0;
  int16_t yPos = 0;
  if (!serialization::readPod(file, xPos) || !serialization::readPod(file, yPos)) return nullptr;

  auto ib = ImageBlock::deserialize(file);
  if (!ib) return nullptr;
  return std::unique_ptr<PageImage>(new (std::nothrow) PageImage(std::move(ib), xPos, yPos));
}

void PageHorizontalRule::render(GfxRenderer& renderer, const int fontId, const int xOffset, const int yOffset) {
  (void)fontId;
  if (width == 0 || thickness == 0) {
    return;
  }

  renderer.drawLine(xPos + xOffset, yPos + yOffset, xPos + xOffset + width - 1, yPos + yOffset, thickness, true);
}

bool PageHorizontalRule::serialize(HalFile& file) {
  serialization::writePod(file, xPos);
  serialization::writePod(file, yPos);
  serialization::writePod(file, width);
  serialization::writePod(file, thickness);
  return true;
}

std::unique_ptr<PageHorizontalRule> PageHorizontalRule::deserialize(HalFile& file) {
  int16_t xPos = 0;
  int16_t yPos = 0;
  uint16_t width = 0;
  uint8_t thickness = 0;
  if (!serialization::readPod(file, xPos) || !serialization::readPod(file, yPos) ||
      !serialization::readPod(file, width) || !serialization::readPod(file, thickness))
    return nullptr;

  if (width == 0 || thickness == 0) {
    LOG_ERR("PGE", "Deserialization failed: invalid horizontal rule metadata (width=%u thickness=%u)", width,
            thickness);
    return nullptr;
  }

  auto* rule = new (std::nothrow) PageHorizontalRule(width, thickness, xPos, yPos);
  if (!rule) {
    LOG_ERR("PGE", "Deserialization failed: could not allocate PageHorizontalRule");
    return nullptr;
  }
  return std::unique_ptr<PageHorizontalRule>(rule);
}

void Page::render(GfxRenderer& renderer, const int fontId, const int xOffset, const int yOffset) const {
  renderFilteredPageElements(elements, renderer, fontId, xOffset, yOffset, [](const PageElement&) { return true; });
}

void Page::renderImages(GfxRenderer& renderer, const int fontId, const int xOffset, const int yOffset) const {
  renderFilteredPageElements(elements, renderer, fontId, xOffset, yOffset,
                             [](const PageElement& element) { return element.getTag() == TAG_PageImage; });
}

bool Page::getSingleImageGeometry(int16_t& x, int16_t& y, int16_t& width, int16_t& height) const {
  if (elements.size() != 1 || elements.front()->getTag() != TAG_PageImage) return false;
  const auto& image = static_cast<const PageImage&>(*elements.front());
  x = image.xPos;
  y = image.yPos;
  width = image.getImageBlock().getWidth();
  height = image.getImageBlock().getHeight();
  return width > 0 && height > 0;
}

bool Page::renderSingleImageViewport(GfxRenderer& renderer, const int xOffset, const int yOffset, const int sourceX,
                                     const int sourceY, const int sourceWidth, const int sourceHeight,
                                     const int destinationWidth, const int destinationHeight) const {
  if (elements.size() != 1 || elements.front()->getTag() != TAG_PageImage) return false;
  auto& image = static_cast<PageImage&>(*elements.front());
  return image.renderViewport(renderer, xOffset, yOffset, sourceX, sourceY, sourceWidth, sourceHeight, destinationWidth,
                              destinationHeight);
}

bool Page::serialize(HalFile& file) const {
  const uint16_t count = elements.size();
  serialization::writePod(file, count);

  for (const auto& el : elements) {
    // Use getTag() method to determine type
    serialization::writePod(file, static_cast<uint8_t>(el->getTag()));

    if (!el->serialize(file)) {
      return false;
    }
  }

  // Serialize footnotes (clamp to MAX_FOOTNOTES_PER_PAGE to match addFootnote/deserialize limits)
  const uint16_t fnCount = std::min<uint16_t>(footnotes.size(), MAX_FOOTNOTES_PER_PAGE);
  serialization::writePod(file, fnCount);
  for (uint16_t i = 0; i < fnCount; i++) {
    const auto& fn = footnotes[i];
    if (file.write(fn.number, sizeof(fn.number)) != sizeof(fn.number) ||
        file.write(fn.href, sizeof(fn.href)) != sizeof(fn.href)) {
      LOG_ERR("PGE", "Failed to write footnote");
      return false;
    }
  }

  return true;
}

std::unique_ptr<Page> Page::deserialize(HalFile& file) {
  auto page = std::unique_ptr<Page>(new (std::nothrow) Page());
  if (!page) {
    LOG_ERR("PGE", "Deserialization failed: could not allocate Page");
    return nullptr;
  }

  uint16_t count = 0;
  if (!serialization::readPod(file, count) || count > MAX_ELEMENTS_PER_PAGE) {
    LOG_ERR("PGE", "Deserialization failed: invalid element count %u", count);
    return nullptr;
  }
  page->elements.reserve(count);

  for (uint16_t i = 0; i < count; i++) {
    uint8_t tag = 0;
    if (!serialization::readPod(file, tag)) return nullptr;

    if (tag == TAG_PageLine) {
      auto pl = PageLine::deserialize(file);
      if (!pl) return nullptr;
      page->elements.push_back(std::move(pl));
    } else if (tag == TAG_PageImage) {
      auto pi = PageImage::deserialize(file);
      if (!pi) return nullptr;
      page->elements.push_back(std::move(pi));
    } else if (tag == TAG_PageHorizontalRule) {
      auto rule = PageHorizontalRule::deserialize(file);
      if (!rule) {
        return nullptr;
      }
      page->elements.push_back(std::move(rule));
    } else {
      LOG_ERR("PGE", "Deserialization failed: Unknown tag %u", tag);
      return nullptr;
    }
  }

  // Deserialize footnotes
  uint16_t fnCount = 0;
  if (!serialization::readPod(file, fnCount) || fnCount > MAX_FOOTNOTES_PER_PAGE) {
    LOG_ERR("PGE", "Invalid footnote count %u", fnCount);
    return nullptr;
  }
  page->footnotes.resize(fnCount);
  for (uint16_t i = 0; i < fnCount; i++) {
    auto& entry = page->footnotes[i];
    if (file.read(entry.number, sizeof(entry.number)) != sizeof(entry.number) ||
        file.read(entry.href, sizeof(entry.href)) != sizeof(entry.href)) {
      LOG_ERR("PGE", "Failed to read footnote %u", i);
      return nullptr;
    }
    entry.number[sizeof(entry.number) - 1] = '\0';
    entry.href[sizeof(entry.href) - 1] = '\0';
  }

  return page;
}
