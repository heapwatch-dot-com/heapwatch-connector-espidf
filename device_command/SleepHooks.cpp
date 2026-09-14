#include "SleepHooks.h"

#include <cstddef>
#include <cstring>

#include "esp_log.h"

namespace heapwatch::sleep_hooks {

namespace {

constexpr char TAG[] = "HwCmd";
constexpr size_t MAX_HOOKS = 4;

struct HookSlot {
    BeforeSleepHook hook = nullptr;
    void* user_ctx = nullptr;
};

HookSlot g_light_hooks[MAX_HOOKS] = {};
HookSlot g_deep_hooks[MAX_HOOKS] = {};
volatile bool g_must_stay_awake = false;

bool registerHook(HookSlot* slots, BeforeSleepHook hook, void* user_ctx) {
    if (hook == nullptr) {
        return false;
    }

    for (size_t i = 0; i < MAX_HOOKS; ++i) {
        if (slots[i].hook == hook && slots[i].user_ctx == user_ctx) {
            return true;
        }
    }

    for (size_t i = 0; i < MAX_HOOKS; ++i) {
        if (slots[i].hook == nullptr) {
            slots[i].hook = hook;
            slots[i].user_ctx = user_ctx;
            return true;
        }
    }

    ESP_LOGE(TAG, "Hook table full");
    return false;
}

bool runHooks(const HookSlot* slots) {
    for (size_t i = 0; i < MAX_HOOKS; ++i) {
        const HookSlot& slot = slots[i];
        if (slot.hook == nullptr) {
            continue;
        }
        if (!slot.hook(slot.user_ctx)) {
            ESP_LOGW(TAG, "Sleep hook vetoed");
            return false;
        }
        if (g_must_stay_awake) {
            ESP_LOGW(TAG, "heapwatch_must_stay_awake() vetoed sleep");
            return false;
        }
    }
    return true;
}

}  // namespace

bool registerBeforeLightSleep(BeforeSleepHook hook, void* user_ctx) {
    return registerHook(g_light_hooks, hook, user_ctx);
}

bool registerBeforeDeepSleep(BeforeSleepHook hook, void* user_ctx) {
    return registerHook(g_deep_hooks, hook, user_ctx);
}

void mustStayAwake() { g_must_stay_awake = true; }

void resetMustStayAwake() { g_must_stay_awake = false; }

bool isMustStayAwake() { return g_must_stay_awake; }

bool runBeforeLightSleepHooks() {
    resetMustStayAwake();
    return runHooks(g_light_hooks);
}

bool runBeforeDeepSleepHooks() {
    resetMustStayAwake();
    return runHooks(g_deep_hooks);
}

}  // namespace heapwatch::sleep_hooks
