// JsonEscape — escape strings for safe embedding into JSON string literals.
//
// Escapes the seven JSON reserved characters per RFC 8259:
//   "  →  \"
//   \  →  \\
//   /         (left as-is; valid either way)
//   \b \f \n \r \t  →  \b \f \n \r \t
// All other ASCII control chars (< 0x20) are silently dropped (NOT
// emitted as \uXXXX) — the badge never produces them in user input,
// and embedding \u escapes would balloon the body buffer.
//
// Threading: pure function, reentrant. Caller owns the output buffer.
//
// History: lifted during spec-010 so SendMessage and any future
// ping-shaped sender can share one impl.

#pragma once

#include <stddef.h>

void jsonEscapeString(const char* src, char* out, size_t outCap);
