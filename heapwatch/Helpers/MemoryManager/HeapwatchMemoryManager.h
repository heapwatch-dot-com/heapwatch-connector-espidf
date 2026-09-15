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
