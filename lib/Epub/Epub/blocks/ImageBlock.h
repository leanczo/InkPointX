#pragma once
#include <HalStorage.h>

#include <memory>
#include <string>

#include "Block.h"

class ImageBlock final : public Block {
 public:
  ImageBlock(const std::string& imagePath, int16_t width, int16_t height);
  ~ImageBlock() override = default;

  const std::string& getImagePath() const { return imagePath; }
  int16_t getWidth() const { return width; }
  int16_t getHeight() const { return height; }

  bool imageExists() const;

  BlockType getType() override { return IMAGE_BLOCK; }
  bool isEmpty() override { return false; }

  void render(GfxRenderer& renderer, const int x, const int y);
  // Render a source rectangle from the decoded pixel cache into arbitrary
  // destination bounds. Used by fixed-layout PDF zoom without decoding the PNG
  // again for every pan step.
  bool renderViewport(GfxRenderer& renderer, int x, int y, int sourceX, int sourceY, int sourceWidth, int sourceHeight,
                      int destinationWidth, int destinationHeight);
  bool serialize(HalFile& file);
  static std::unique_ptr<ImageBlock> deserialize(HalFile& file);

 private:
  std::string imagePath;
  int16_t width;
  int16_t height;
};
