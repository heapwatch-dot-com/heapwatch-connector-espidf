// Copyright 2026 Heapwatch
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstddef>

namespace Heapwatch::Helpers {

/**
 * Canonical middle segment for dynamic /parameter keys:
 * lowercase, runs of non [a-z0-9] become a single '_', trim '_'.
 * Returns false if the result is empty.
 */
bool normalizeDynamicSegment(const char* in, char* out, size_t out_size);

}  // namespace Heapwatch::Helpers
