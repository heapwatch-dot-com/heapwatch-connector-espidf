// Copyright 2026 Heapwatch
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstddef>

#include "heapwatch/Helpers/MemoryManager/ConnectorMemoryBlock.h"

namespace Heapwatch::Helpers::MemoryManager {

/** One heap alloc of the memory block + static FreeRTOS objects. Abort on failure. No-op if already inited. */
void init();

/**
 * Delete static RTOS objects in the memory block and free the blob.
 * Caller must have stopped every task that uses it. No-op if not inited.
 */
void deinit();

/** nullptr before init and after deinit. */
ConnectorMemoryBlock* get();

/** 8-byte aligned bump from temporary_arena[]. Returns nullptr if the arena is full. */
void* temporaryArenaAlloc(size_t bytes);

void temporaryArenaReset();

}  // namespace Heapwatch::Helpers::MemoryManager
