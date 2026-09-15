#include "heapwatch/Lifecycle/BootCycle/BootCycleStore.h"

#include <cstdint>
#include <cinttypes>

#include "esp_log.h"
#include "esp_system.h"
#include "nvs.h"

namespace Heapwatch::Lifecycle::BootCycle {

namespace {
constexpr char TAG[] = "HwLife";
constexpr char NVS_NAMESPACE[] = "heapwatch";
constexpr char KEY_BOOT_CYCLE[] = "boot_cycle";
// NVS keys are max 15 chars (NVS_KEY_NAME_MAX_SIZE-1). "last_crash_boot_cycle"
// is 21 and caused ESP_ERR_NVS_KEY_TOO_LONG → abort reboot loop.
constexpr char KEY_LAST_CRASH_BOOT_CYCLE[] = "last_crash_bc";
}  // namespace

bool loadIncrementAndPersistBootCycle(State* out) {
    if (out == nullptr) {
        return false;
    }

    nvs_handle_t handle = 0;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS namespace '%s'", NVS_NAMESPACE);
        return false;
    }

    uint32_t boot_cycle = 0;
    uint32_t last_crash_boot_cycle = 0;
    nvs_get_u32(handle, KEY_BOOT_CYCLE, &boot_cycle);
    nvs_get_u32(handle, KEY_LAST_CRASH_BOOT_CYCLE, &last_crash_boot_cycle);

    if (esp_reset_reason() == ESP_RST_PANIC && last_crash_boot_cycle == 0) {
        last_crash_boot_cycle = boot_cycle;
        ESP_LOGW(TAG, "PANIC boot: attributed crash to boot_cycle=%" PRIu32,
                 static_cast<uint32_t>(boot_cycle));
    }

    boot_cycle++;

    esp_err_t err = nvs_set_u32(handle, KEY_BOOT_CYCLE, boot_cycle);
    if (err == ESP_OK) {
        err = nvs_set_u32(handle, KEY_LAST_CRASH_BOOT_CYCLE, last_crash_boot_cycle);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }

    nvs_close(handle);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to persist boot cycle state: %s", esp_err_to_name(err));
        return false;
    }

    out->boot_cycle = boot_cycle;
    out->last_crash_boot_cycle = last_crash_boot_cycle;

    ESP_LOGI(TAG, "boot_cycle=%" PRIu32 " last_crash_boot_cycle=%" PRIu32,
             static_cast<uint32_t>(out->boot_cycle),
             static_cast<uint32_t>(out->last_crash_boot_cycle));
    return true;
}

bool clearLastCrash() {
    nvs_handle_t handle = 0;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS to clear last_crash_boot_cycle");
        return false;
    }

    esp_err_t err = nvs_erase_key(handle, KEY_LAST_CRASH_BOOT_CYCLE);
    if (err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND) {
        err = nvs_commit(handle);
    }

    nvs_close(handle);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to clear last_crash_boot_cycle: %s", esp_err_to_name(err));
        return false;
    }

    ESP_LOGI(TAG, "Cleared last_crash_boot_cycle after successful core-dump upload");
    return true;
}

bool resumeFromServerLast(uint32_t server_last_boot_cycle, State* in_out) {
    if (in_out == nullptr || server_last_boot_cycle == UINT32_MAX) {
        return false;
    }

    if (in_out->boot_cycle > server_last_boot_cycle) {
        return false;
    }

    const uint32_t resumed = server_last_boot_cycle + 1U;
    if (resumed == in_out->boot_cycle) {
        return false;
    }

    nvs_handle_t handle = 0;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS to resume boot_cycle");
        return false;
    }

    esp_err_t err = nvs_set_u32(handle, KEY_BOOT_CYCLE, resumed);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to persist resumed boot_cycle: %s", esp_err_to_name(err));
        return false;
    }

    ESP_LOGW(TAG, "Resumed boot_cycle from server last=%" PRIu32 " -> %" PRIu32,
             static_cast<uint32_t>(server_last_boot_cycle),
             static_cast<uint32_t>(resumed));
    in_out->boot_cycle = resumed;
    return true;
}

}  // namespace Heapwatch::Lifecycle::BootCycle
