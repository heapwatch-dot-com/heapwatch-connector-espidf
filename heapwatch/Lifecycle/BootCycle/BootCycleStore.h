#pragma once

#include <cstdint>

namespace Heapwatch::Lifecycle::BootCycle {

/** Persisted boot counter state (see device-sessions findings §21). */
struct State {
    /** Monotonic boot counter for the current session (incremented once per boot). */
    uint32_t boot_cycle = 0;
    /**
     * Boot cycle of the session that crashed, if known. 0 means unset.
     * Set at boot time when esp_reset_reason() == ESP_RST_PANIC.
     */
    uint32_t last_crash_boot_cycle = 0;
};

/**
 * Read NVS, apply PANIC attribution, increment boot_cycle, persist.
 * Must be called once per boot after nvs_flash_init().
 */
bool onBoot(State* out);

/** Clear last_crash_boot_cycle after a successful core-dump upload. */
bool clearLastCrash();

/**
 * When NVS boot_cycle was reset but the device still has fleet credentials, align the
 * local counter with the server's last known boot_cycle (+1 for this boot).
 */
bool resumeFromServerLast(uint32_t server_last_boot_cycle, State* in_out);

}  // namespace Heapwatch::Lifecycle::BootCycle
