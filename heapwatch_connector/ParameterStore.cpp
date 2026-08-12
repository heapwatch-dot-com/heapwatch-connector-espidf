// Copyright (c) 2026 Viktor Jamrich Enterprises s.r.o. All rights reserved.
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
// For licensing inquiries, please contact: admin@heapwatch.com
//

#include "ParameterStore.h"

#include <cstdio>
#include <cstring>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "heapwatch_config.h"
#include "heapwatch_connector_timing.h"

namespace heapwatch::parameter_store {

namespace {

constexpr size_t kMaxSlugLen = 48;

struct ParamSlot {
    bool in_use = false;
    char slug[kMaxSlugLen] = {0};
    bool is_double = false;

    int64_t int_value = 0;
    double double_value = 0.0;

    bool has_bounds = false;
    int64_t int_min = 0;
    int64_t int_max = 0;
    double double_min = 0.0;
    double double_max = 0.0;

    // PARAM1b: dirty-flag-gated resend -- true until the current bounds have
    // actually been included in a flush once.
    bool bounds_dirty = true;
    TickType_t last_bounds_sent_tick = 0;
};

ParamSlot g_slots[HEAPWATCH_MAX_PARAMETER_SLOTS];
SemaphoreHandle_t g_mutex = nullptr;

SemaphoreHandle_t get_or_create_mutex() {
    if (g_mutex == nullptr) {
        g_mutex = xSemaphoreCreateMutex();
    }
    return g_mutex;
}

// Not thread-safe by itself -- callers must hold g_mutex.
ParamSlot* find_or_create_slot_locked(const char* slug) {
    if (slug == nullptr || slug[0] == '\0') {
        return nullptr;
    }

    for (size_t i = 0; i < HEAPWATCH_MAX_PARAMETER_SLOTS; ++i) {
        if (g_slots[i].in_use && strncmp(g_slots[i].slug, slug, kMaxSlugLen) == 0) {
            return &g_slots[i];
        }
    }

    for (size_t i = 0; i < HEAPWATCH_MAX_PARAMETER_SLOTS; ++i) {
        if (!g_slots[i].in_use) {
            g_slots[i] = ParamSlot();
            g_slots[i].in_use = true;
            snprintf(g_slots[i].slug, sizeof(g_slots[i].slug), "%s", slug);
            return &g_slots[i];
        }
    }

    // HEAPWATCH_MAX_PARAMETER_SLOTS exhausted -- this slug is silently dropped.
    // No malloc/free is a deliberate footprint choice (see task_plan.md M1/M0);
    // a client registering more distinct slugs than this needs a larger
    // HEAPWATCH_MAX_PARAMETER_SLOTS, not a dynamic fallback.
    return nullptr;
}

// Bounds are re-included in a flush when they differ from what was last sent,
// OR the rare safety-net resend interval has elapsed (PARAM1b) -- whichever
// path fired, the caller is expected to mark the bounds as sent afterward.
bool bounds_due_for_resend_locked(const ParamSlot& slot, TickType_t now) {
    if (!slot.has_bounds) {
        return false;
    }
    if (slot.bounds_dirty) {
        return true;
    }
    const TickType_t elapsed_ticks = now - slot.last_bounds_sent_tick;
    return elapsed_ticks >= pdMS_TO_TICKS(HEAPWATCH_PARAMETER_BOUNDS_RESEND_INTERVAL_MS);
}

}  // namespace

void updateInt(const char* slug, int64_t value) {
    SemaphoreHandle_t mutex = get_or_create_mutex();
    if (mutex == nullptr || xSemaphoreTake(mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return;
    }
    ParamSlot* slot = find_or_create_slot_locked(slug);
    if (slot != nullptr) {
        slot->is_double = false;
        slot->int_value = value;
    }
    xSemaphoreGive(mutex);
}

void updateDouble(const char* slug, double value) {
    SemaphoreHandle_t mutex = get_or_create_mutex();
    if (mutex == nullptr || xSemaphoreTake(mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return;
    }
    ParamSlot* slot = find_or_create_slot_locked(slug);
    if (slot != nullptr) {
        slot->is_double = true;
        slot->double_value = value;
    }
    xSemaphoreGive(mutex);
}

void updateIntRanged(const char* slug, int64_t value, int64_t min_value, int64_t max_value) {
    SemaphoreHandle_t mutex = get_or_create_mutex();
    if (mutex == nullptr || xSemaphoreTake(mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return;
    }
    ParamSlot* slot = find_or_create_slot_locked(slug);
    if (slot != nullptr) {
        slot->is_double = false;
        slot->int_value = value;
        if (!slot->has_bounds || slot->int_min != min_value || slot->int_max != max_value) {
            slot->bounds_dirty = true;
        }
        slot->has_bounds = true;
        slot->int_min = min_value;
        slot->int_max = max_value;
    }
    xSemaphoreGive(mutex);
}

void updateDoubleRanged(const char* slug, double value, double min_value, double max_value) {
    SemaphoreHandle_t mutex = get_or_create_mutex();
    if (mutex == nullptr || xSemaphoreTake(mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return;
    }
    ParamSlot* slot = find_or_create_slot_locked(slug);
    if (slot != nullptr) {
        slot->is_double = true;
        slot->double_value = value;
        if (!slot->has_bounds || slot->double_min != min_value || slot->double_max != max_value) {
            slot->bounds_dirty = true;
        }
        slot->has_bounds = true;
        slot->double_min = min_value;
        slot->double_max = max_value;
    }
    xSemaphoreGive(mutex);
}

bool buildFlushPayload(char* out, size_t out_size) {
    if (out == nullptr || out_size == 0) {
        return false;
    }

    SemaphoreHandle_t mutex = get_or_create_mutex();
    if (mutex == nullptr || xSemaphoreTake(mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return false;
    }

    bool any_slot_in_use = false;
    for (size_t i = 0; i < HEAPWATCH_MAX_PARAMETER_SLOTS; ++i) {
        if (g_slots[i].in_use) {
            any_slot_in_use = true;
            break;
        }
    }
    if (!any_slot_in_use) {
        xSemaphoreGive(mutex);
        return false;
    }

    uint64_t tsms = 0;
    uint64_t utms = 0;
    heapwatch_get_ingress_timing_ms(&tsms, &utms);
    const TickType_t now = xTaskGetTickCount();

    size_t pos = 0;
    pos += snprintf(out + pos, out_size - pos, "{\"tsms\":%llu,\"utms\":%llu",
                     static_cast<unsigned long long>(tsms), static_cast<unsigned long long>(utms));

    for (size_t i = 0; i < HEAPWATCH_MAX_PARAMETER_SLOTS && pos < out_size; ++i) {
        ParamSlot& slot = g_slots[i];
        if (!slot.in_use) {
            continue;
        }

        if (slot.is_double) {
            pos += snprintf(out + pos, out_size - pos, ",\"%s\":%.6f", slot.slug, slot.double_value);
        } else {
            pos += snprintf(out + pos, out_size - pos, ",\"%s\":%lld", slot.slug,
                             static_cast<long long>(slot.int_value));
        }

        if (bounds_due_for_resend_locked(slot, now) && pos < out_size) {
            if (slot.is_double) {
                pos += snprintf(out + pos, out_size - pos, ",\"%s_min\":%.6f,\"%s_max\":%.6f",
                                 slot.slug, slot.double_min, slot.slug, slot.double_max);
            } else {
                pos += snprintf(out + pos, out_size - pos, ",\"%s_min\":%lld,\"%s_max\":%lld",
                                 slot.slug, static_cast<long long>(slot.int_min), slot.slug,
                                 static_cast<long long>(slot.int_max));
            }
            slot.bounds_dirty = false;
            slot.last_bounds_sent_tick = now;
        }
    }

    if (pos < out_size) {
        out[pos++] = '}';
    }
    if (pos < out_size) {
        out[pos] = '\0';
    } else {
        out[out_size - 1] = '\0';
    }

    xSemaphoreGive(mutex);
    return true;
}

}  // namespace heapwatch::parameter_store
