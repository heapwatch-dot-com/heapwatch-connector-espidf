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
// For licensing inquiries, please contact: admin@heapwatch.com
//

#include "ParameterStore.h"

#include <cstdint>
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
    char slug[kMaxSlugLen] = {0};
    int64_t int_value = 0;
    double double_value = 0.0;
    int64_t int_min = 0;
    int64_t int_max = 0;
    double double_min = 0.0;
    double double_max = 0.0;
    TickType_t last_bounds_sent_tick = 0;
    uint8_t flags = 0;
};

constexpr uint8_t kSlotInUse = 1 << 0;
constexpr uint8_t kSlotIsDouble = 1 << 1;
constexpr uint8_t kSlotHasValue = 1 << 2;
constexpr uint8_t kSlotHasMin = 1 << 3;
constexpr uint8_t kSlotHasMax = 1 << 4;
constexpr uint8_t kSlotBoundsDirty = 1 << 5;

bool slot_flag(const ParamSlot& slot, uint8_t bit) {
    return (slot.flags & bit) != 0;
}

void slot_set_flag(ParamSlot& slot, uint8_t bit, bool on) {
    if (on) {
        slot.flags = static_cast<uint8_t>(slot.flags | bit);
    } else {
        slot.flags = static_cast<uint8_t>(slot.flags & static_cast<uint8_t>(~bit));
    }
}

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
        if (slot_flag(g_slots[i], kSlotInUse) && strncmp(g_slots[i].slug, slug, kMaxSlugLen) == 0) {
            return &g_slots[i];
        }
    }

    for (size_t i = 0; i < HEAPWATCH_MAX_PARAMETER_SLOTS; ++i) {
        if (!slot_flag(g_slots[i], kSlotInUse)) {
            g_slots[i] = ParamSlot();
            slot_set_flag(g_slots[i], kSlotInUse, true);
            slot_set_flag(g_slots[i], kSlotBoundsDirty, true);
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

bool slot_has_publishable_content_locked(const ParamSlot& slot) {
    return slot_flag(slot, kSlotHasValue) || slot_flag(slot, kSlotBoundsDirty);
}

}  // namespace

void updateInt(const char* slug, int64_t value) {
    SemaphoreHandle_t mutex = get_or_create_mutex();
    if (mutex == nullptr || xSemaphoreTake(mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return;
    }
    ParamSlot* slot = find_or_create_slot_locked(slug);
    if (slot != nullptr) {
        slot_set_flag(*slot, kSlotIsDouble, false);
        slot_set_flag(*slot, kSlotHasValue, true);
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
        slot_set_flag(*slot, kSlotIsDouble, true);
        slot_set_flag(*slot, kSlotHasValue, true);
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
        slot_set_flag(*slot, kSlotIsDouble, false);
        slot_set_flag(*slot, kSlotHasValue, true);
        slot->int_value = value;
        if (!slot_flag(*slot, kSlotHasMin) || !slot_flag(*slot, kSlotHasMax)
            || slot->int_min != min_value || slot->int_max != max_value) {
            slot_set_flag(*slot, kSlotBoundsDirty, true);
        }
        slot_set_flag(*slot, kSlotHasMin, true);
        slot_set_flag(*slot, kSlotHasMax, true);
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
        slot_set_flag(*slot, kSlotIsDouble, true);
        slot_set_flag(*slot, kSlotHasValue, true);
        slot->double_value = value;
        if (!slot_flag(*slot, kSlotHasMin) || !slot_flag(*slot, kSlotHasMax)
            || slot->double_min != min_value || slot->double_max != max_value) {
            slot_set_flag(*slot, kSlotBoundsDirty, true);
        }
        slot_set_flag(*slot, kSlotHasMin, true);
        slot_set_flag(*slot, kSlotHasMax, true);
        slot->double_min = min_value;
        slot->double_max = max_value;
    }
    xSemaphoreGive(mutex);
}

void setIntMax(const char* slug, int64_t max_value) {
    SemaphoreHandle_t mutex = get_or_create_mutex();
    if (mutex == nullptr || xSemaphoreTake(mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return;
    }
    ParamSlot* slot = find_or_create_slot_locked(slug);
    if (slot != nullptr) {
        if (!slot_flag(*slot, kSlotHasValue)) {
            slot_set_flag(*slot, kSlotIsDouble, false);
        }
        if (!slot_flag(*slot, kSlotIsDouble)) {
            if (!slot_flag(*slot, kSlotHasMax) || slot->int_max != max_value) {
                slot_set_flag(*slot, kSlotBoundsDirty, true);
            }
            slot_set_flag(*slot, kSlotHasMax, true);
            slot->int_max = max_value;
        }
    }
    xSemaphoreGive(mutex);
}

void setIntMin(const char* slug, int64_t min_value) {
    SemaphoreHandle_t mutex = get_or_create_mutex();
    if (mutex == nullptr || xSemaphoreTake(mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return;
    }
    ParamSlot* slot = find_or_create_slot_locked(slug);
    if (slot != nullptr) {
        if (!slot_flag(*slot, kSlotHasValue)) {
            slot_set_flag(*slot, kSlotIsDouble, false);
        }
        if (!slot_flag(*slot, kSlotIsDouble)) {
            if (!slot_flag(*slot, kSlotHasMin) || slot->int_min != min_value) {
                slot_set_flag(*slot, kSlotBoundsDirty, true);
            }
            slot_set_flag(*slot, kSlotHasMin, true);
            slot->int_min = min_value;
        }
    }
    xSemaphoreGive(mutex);
}

void setDoubleMax(const char* slug, double max_value) {
    SemaphoreHandle_t mutex = get_or_create_mutex();
    if (mutex == nullptr || xSemaphoreTake(mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return;
    }
    ParamSlot* slot = find_or_create_slot_locked(slug);
    if (slot != nullptr) {
        if (!slot_flag(*slot, kSlotHasValue)) {
            slot_set_flag(*slot, kSlotIsDouble, true);
        }
        if (slot_flag(*slot, kSlotIsDouble)) {
            if (!slot_flag(*slot, kSlotHasMax) || slot->double_max != max_value) {
                slot_set_flag(*slot, kSlotBoundsDirty, true);
            }
            slot_set_flag(*slot, kSlotHasMax, true);
            slot->double_max = max_value;
        }
    }
    xSemaphoreGive(mutex);
}

void setDoubleMin(const char* slug, double min_value) {
    SemaphoreHandle_t mutex = get_or_create_mutex();
    if (mutex == nullptr || xSemaphoreTake(mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return;
    }
    ParamSlot* slot = find_or_create_slot_locked(slug);
    if (slot != nullptr) {
        if (!slot_flag(*slot, kSlotHasValue)) {
            slot_set_flag(*slot, kSlotIsDouble, true);
        }
        if (slot_flag(*slot, kSlotIsDouble)) {
            if (!slot_flag(*slot, kSlotHasMin) || slot->double_min != min_value) {
                slot_set_flag(*slot, kSlotBoundsDirty, true);
            }
            slot_set_flag(*slot, kSlotHasMin, true);
            slot->double_min = min_value;
        }
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

    uint64_t tsms = 0;
    uint64_t utms = 0;
    heapwatch_get_ingress_timing_ms(&tsms, &utms);

    bool any_publishable = false;
    for (size_t i = 0; i < HEAPWATCH_MAX_PARAMETER_SLOTS; ++i) {
        if (slot_flag(g_slots[i], kSlotInUse) && slot_has_publishable_content_locked(g_slots[i])) {
            any_publishable = true;
            break;
        }
    }
    if (!any_publishable) {
        xSemaphoreGive(mutex);
        return false;
    }

    size_t pos = 0;
    pos += snprintf(out + pos, out_size - pos, "{\"tsms\":%llu,\"utms\":%llu",
                     static_cast<unsigned long long>(tsms), static_cast<unsigned long long>(utms));

    for (size_t i = 0; i < HEAPWATCH_MAX_PARAMETER_SLOTS && pos < out_size; ++i) {
        ParamSlot& slot = g_slots[i];
        if (!slot_flag(slot, kSlotInUse)) {
            continue;
        }

        if (slot_flag(slot, kSlotHasValue)) {
            if (slot_flag(slot, kSlotIsDouble)) {
                pos += snprintf(out + pos, out_size - pos, ",\"%s\":%.6f", slot.slug, slot.double_value);
            } else {
                pos += snprintf(out + pos, out_size - pos, ",\"%s\":%lld", slot.slug,
                                 static_cast<long long>(slot.int_value));
            }
        }

        if (slot_flag(slot, kSlotBoundsDirty) && pos < out_size) {
            if (slot_flag(slot, kSlotIsDouble)) {
                if (slot_flag(slot, kSlotHasMin)) {
                    pos += snprintf(out + pos, out_size - pos, ",\"%s_min\":%.6f", slot.slug,
                                     slot.double_min);
                }
                if (slot_flag(slot, kSlotHasMax) && pos < out_size) {
                    pos += snprintf(out + pos, out_size - pos, ",\"%s_max\":%.6f", slot.slug,
                                     slot.double_max);
                }
            } else {
                if (slot_flag(slot, kSlotHasMin)) {
                    pos += snprintf(out + pos, out_size - pos, ",\"%s_min\":%lld", slot.slug,
                                     static_cast<long long>(slot.int_min));
                }
                if (slot_flag(slot, kSlotHasMax) && pos < out_size) {
                    pos += snprintf(out + pos, out_size - pos, ",\"%s_max\":%lld", slot.slug,
                                     static_cast<long long>(slot.int_max));
                }
            }
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

void acknowledgeBoundsSent() {
    SemaphoreHandle_t mutex = get_or_create_mutex();
    if (mutex == nullptr || xSemaphoreTake(mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return;
    }

    const TickType_t now = xTaskGetTickCount();
    for (size_t i = 0; i < HEAPWATCH_MAX_PARAMETER_SLOTS; ++i) {
        ParamSlot& slot = g_slots[i];
        if (!slot_flag(slot, kSlotInUse) || !slot_flag(slot, kSlotBoundsDirty)) {
            continue;
        }
        slot_set_flag(slot, kSlotBoundsDirty, false);
        slot.last_bounds_sent_tick = now;
    }

    xSemaphoreGive(mutex);
}

}  // namespace heapwatch::parameter_store
