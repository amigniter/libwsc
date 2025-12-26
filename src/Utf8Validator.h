/*
 *  Utf8Validator.h
 *  Author: Milan M.
 *  Copyright (c) 2025 AMSOFTSWITCH LTD. All rights reserved.
 */

#pragma once
#include <cstdint>
#include <cstddef>

class Utf8Validator {
public:
    Utf8Validator();
    bool validateChunk(const uint8_t* data, size_t len);
    bool validateFinal() const;
    void reset();

private:
    int  expectedContinuation;
    bool seenE0;
    bool seenED;
    bool seenF0;
    bool seenF4;
};
