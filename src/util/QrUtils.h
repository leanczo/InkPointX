#pragma once

#include <GfxRenderer.h>

#include <string>

#include "components/themes/BaseTheme.h"

namespace QrUtils {

// Renders a QR code with the given text payload within the specified bounding box.
// Returns false when the encoder refuses the payload (nothing is drawn).
// *wasTruncated is set when the payload exceeded QR capacity and only a prefix
// was encoded — callers should tell the user the code is partial.
bool drawQrCode(const GfxRenderer& renderer, const Rect& bounds, const std::string& textPayload,
                bool* wasTruncated = nullptr);

}  // namespace QrUtils
