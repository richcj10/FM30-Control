#pragma once
/*
 * Text clean-up for anything that ends up in RDS (PS/RT) or on the LCD.
 *
 * The STM32 takes printable ASCII. Network text is usually UTF-8 (sometimes
 * Windows-1252), so convert it:
 *   - CR, LF, TAB and other control characters become a single space
 *   - common accented Latin letters and typographic punctuation are transliterated
 *     ("Björk – Jóga" -> "Bjork - Joga")
 *   - anything else becomes '?'
 *   - '|' is removed when stripPipe is set (it's the LCD MSG top|bottom separator)
 *   - leading/trailing spaces are trimmed, then the result is clipped to maxChars
 */
#include <stddef.h>

namespace fm30 {

// out must hold maxChars + 1 bytes. Returns the output length.
size_t sanitizeText(const char *in, char *out, size_t maxChars, bool stripPipe);

}  // namespace fm30
