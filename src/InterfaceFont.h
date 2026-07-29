#pragma once

// Rebinds the two stable UI font IDs to the family selected in SETTINGS.
// The font data is already resident in flash; this does not allocate another
// framebuffer or duplicate glyph data.
void applyInterfaceFont();
