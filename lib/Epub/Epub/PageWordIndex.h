#pragma once

#include <EpdFontFamily.h>
#include <Epub/Page.h>

#include <cstddef>
#include <string>
#include <vector>

class GfxRenderer;

struct PageWordHit {
  size_t elementIndex = 0;
  size_t wordIndexInElement = 0;
  int screenX = 0;
  int screenY = 0;
  int screenW = 0;
  int screenH = 0;
  int fontId = 0;
  EpdFontFamily::Style style = EpdFontFamily::REGULAR;
  std::string text;
};

void buildPageWordIndex(const Page& page, const GfxRenderer& renderer, int bodyFontId, int marginLeft, int marginTop,
                        std::vector<PageWordHit>& out, std::vector<size_t>* lineStartsOut = nullptr);
