// Copyright (c) 2026 Heapwatch. All rights reserved.

#include "DynamicParameterSegment.h"

#include <cstring>

namespace heapwatch {

bool normalizeDynamicSegment(const char* in, char* out, size_t out_size) {
    if (in == nullptr || out == nullptr || out_size == 0) {
        if (out != nullptr && out_size > 0) {
            out[0] = '\0';
        }
        return false;
    }

    char tmp[128];
    size_t t = 0;
    bool last_us = false;

    for (size_t i = 0; in[i] != '\0' && t + 1 < sizeof(tmp); ++i) {
        unsigned char c = static_cast<unsigned char>(in[i]);
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<unsigned char>(c - 'A' + 'a');
        }

        const bool alnum = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        if (alnum) {
            tmp[t++] = static_cast<char>(c);
            last_us = false;
        } else if (t > 0 && !last_us) {
            tmp[t++] = '_';
            last_us = true;
        }
    }

    while (t > 0 && tmp[t - 1] == '_') {
        --t;
    }
    tmp[t] = '\0';

    if (t == 0 || t >= out_size) {
        out[0] = '\0';
        return false;
    }

    memcpy(out, tmp, t + 1);
    return true;
}

}  // namespace heapwatch
