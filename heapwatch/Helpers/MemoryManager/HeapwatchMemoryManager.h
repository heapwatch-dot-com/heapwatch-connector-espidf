// Copyright (c) 2026 Heapwatch. All rights reserved.
//
// This software is part of the Heapwatch Connector for ESP-IDF project.
// Unauthorized copying, modification, or distribution of this software,
// via any medium, is strictly prohibited unless otherwise permitted.
//
// This software is provided "as is", without warranty of any kind, express or implied,
// including but not limited to the warranties of merchantability, fitness for a particular purpose,
// and noninfringement. In no event shall the authors or copyright holders be liable for any claim,
// damages, or other liability, whether in an action of contract, tort, or otherwise, arising from,
// out of, or in connection with the software or the use or other dealings in the software.
//
// For licensing inquiries, please contact: info@heapwatch.com
//

#pragma once

#include <cstddef>

#include "heapwatch/Helpers/MemoryManager/HeapwatchWorkingSet.h"

namespace Heapwatch::Helpers::MemoryManager {

/** One heap alloc of the working set + static FreeRTOS objects. Abort on failure. No-op if already inited. */
void init();

/**
 * Delete static RTOS objects in the working set and free the blob.
 * Caller must have stopped every task that uses it. No-op if not inited.
 */
void deinit();

/** nullptr before init and after deinit. */
HeapwatchWorkingSet* get();

/** 8-byte aligned bump from scratch[]. Returns nullptr if the arena is full. */
void* scratchAlloc(size_t bytes);

void scratchReset();

}  // namespace Heapwatch::Helpers::MemoryManager
