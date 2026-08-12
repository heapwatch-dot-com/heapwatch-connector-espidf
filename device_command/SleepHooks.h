#pragma once

#include <cstddef>
#include <cstdint>

namespace heapwatch::sleep_hooks {

using BeforeSleepHook = bool (*)(void* user_ctx);

/** Register a hook run before light sleep (fixed table, max 4). */
bool registerBeforeLightSleep(BeforeSleepHook hook, void* user_ctx);

/** Register a hook run before deep sleep (fixed table, max 4). */
bool registerBeforeDeepSleep(BeforeSleepHook hook, void* user_ctx);

/** Called from hook code to veto the current sleep command (no timeout). */
void mustStayAwake();

void resetMustStayAwake();

bool isMustStayAwake();

/** Run all hooks for the sleep kind. False if any hook returns false. */
bool runBeforeLightSleepHooks();
bool runBeforeDeepSleepHooks();

}  // namespace heapwatch::sleep_hooks
