/*
 *  Utf8Validator.cpp
 *  Author: Milan M.
 *  Copyright (c) 2025 AMSOFTSWITCH LTD. All rights reserved.
 */

#include "Utf8Validator.h"

Utf8Validator::Utf8Validator()
        : expectedContinuation(0),
            seenE0(false), seenED(false),
            seenF0(false), seenF4(false) {};

/**
 * \brief Validate a chunk of UTF-8 encoded data.
 *
 * Processes input bytes in a streaming fashion, maintaining state between chunks.
 * Validates UTF-8 sequences according to RFC 3629, checking for:
 * - Proper byte sequences
 * - Overlong encodings
 * - Invalid code point ranges
 * - Proper continuation bytes
 *
 * \param data Pointer to the byte buffer to validate
 * \param len  Number of bytes in the buffer
 * \return true if all bytes are valid UTF-8, false if any invalid sequence is found
 *
 * \details Validation rules:
 * 1. When no continuation is expected:
 *    - ASCII (0x00-0x7F): always valid
 *    - 2-byte lead (0xC2-0xDF): expect 1 continuation
 *    - 3-byte lead (0xE0-0xEF):
 *      - Record E0/ED special cases
 *      - Expect 2 continuations
 *    - 4-byte lead (0xF0-0xF4):
 *      - Record F0/F4 special cases
 *      - Expect 3 continuations
 *    - Other lead bytes: invalid
 *
 * 2. When continuation is expected:
 *    - Byte must be 0x80-0xBF
 *    - Apply special case checks:
 *      - E0: second byte ≥ 0xA0 (no overlong)
 *      - ED: second byte ≤ 0x9F (no surrogates)
 *      - F0: second byte ≥ 0x90 (no overlong)
 *      - F4: second byte ≤ 0x8F (≤ U+10FFFF)
 *    - Decrement continuation count
 *    - Clear flags when sequence completes
 */
bool Utf8Validator::validateChunk(const uint8_t* data, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        uint8_t b = data[i];
        if (expectedContinuation == 0) {
            if (b < 0x80) {
                continue;
            }
            else if ((b >> 5) == 0x6) {
                if (b < 0xC2 || b > 0xDF) return false;
                expectedContinuation = 1;
            }
            else if ((b >> 4) == 0xE) {
                seenE0 = (b == 0xE0);
                seenED = (b == 0xED);
                expectedContinuation = 2;
            }
            else if ((b >> 3) == 0x1E) {
                seenF0 = (b == 0xF0);
                seenF4 = (b == 0xF4);
                // allow only up to F4
                if (b < 0xF0 || b > 0xF4) return false;
                expectedContinuation = 3;
            }
            else {
                return false;
            }
        }
        else {
            // Continuation byte
            if ((b >> 6) != 0x2) {
                return false;
            }
            if (seenE0 && expectedContinuation == 2 && b < 0xA0) {
                return false;
            }
            if (seenED && expectedContinuation == 2 && b > 0x9F) {
                return false;
            }
            if (seenF0 && expectedContinuation == 3 && b < 0x90) {
                return false;
            }
            if (seenF4 && expectedContinuation == 3 && b > 0x8F) {
                return false;
            }

            --expectedContinuation;
            if (expectedContinuation == 0) {
                seenE0 = seenED = seenF0 = seenF4 = false;
            }
        }
    }
    return true;
}

/**
 * \brief Final validation check for UTF-8 stream completeness.
 * 
 * \return true if the stream ended on a complete code point (no pending continuation bytes),
 *         false if an incomplete multi-byte sequence was truncated
 *
 * \details This must be called after processing the final chunk of data to verify that:
 * - No partial UTF-8 sequences remain
 * - The stream ended on a complete code point boundary
 * - Equivalent to checking expectedContinuation == 0
 */
bool Utf8Validator::validateFinal() const {
    return expectedContinuation == 0;
}

/**
 * \brief Reset the validator to its initial state.
 *
 * \details Clears all internal state including:
 * - Pending continuation byte expectations
 * - Any recorded special case flags (F4, ED, etc.)
 * - Prepares the validator for a new UTF-8 validation session
 */
void Utf8Validator::reset() {
    expectedContinuation = 0;
    seenE0 = seenED = seenF0 = seenF4 = false;
}
