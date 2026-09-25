#include "heapwatch/Settings/ConnectorSettings.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <cinttypes>

#include "esp_log.h"
#include "nvs.h"

#include "heapwatch_config.h"

namespace Heapwatch::Settings {

namespace {

constexpr char TAG[] = "HwLife";
constexpr char NVS_NAMESPACE[] = "heapwatch";
constexpr char MASKED_SECRET[] = "••••••";

constexpr char KEY_TEL_INT_MS[] = "tel_int_ms";
constexpr char KEY_LOG_LVL[] = "log_lvl";
constexpr char KEY_OUT_DRAIN_MS[] = "out_drain_ms";
constexpr char KEY_TENANT_SLUG[] = "tenant_slug";
constexpr char KEY_DEVICE_UID[] = "device_uid";
constexpr char KEY_MQTT_PASSWORD[] = "mqtt_password";
constexpr char KEY_DEVICE_API_KEY[] = "device_api_key";
constexpr char KEY_STREAM_API_KEY[] = "stream_api_key";
constexpr char KEY_PENDING_UID[] = "pending_uid";
constexpr char KEY_PROVISIONED[] = "provisioned";
constexpr char KEY_BOOT_CYCLE[] = "boot_cycle";
constexpr char KEY_LAST_CRASH_BC[] = "last_crash_bc";

constexpr uint32_t DEFAULT_OUTBOX_DRAIN_MS = 30000;
constexpr uint32_t MIN_TELEMETRY_MS = 1000;
constexpr uint32_t MAX_TELEMETRY_MS = 86400000;
constexpr uint32_t MIN_OUTBOX_DRAIN_MS = 1000;
constexpr uint32_t MAX_OUTBOX_DRAIN_MS = 120000;
constexpr size_t MAX_MANAGED_CATALOGS = 4;
constexpr size_t MAX_CATALOG_FIELDS = 32;

constexpr FieldSchema HEAPWATCH_FIELDS[] = {
    {KEY_TEL_INT_MS, "Telemetry interval",
     "Interval between automatic telemetry metric collection cycles.", "int", false, true, "ms",
     static_cast<int32_t>(MIN_TELEMETRY_MS), static_cast<int32_t>(MAX_TELEMETRY_MS)},
    {KEY_LOG_LVL, "Log relay level",
     "Minimum ESP-IDF log severity relayed to Heapwatch over MQTT.", "enum", false, true,
     nullptr, HEAPWATCH_SEVERITY_DEBUG, HEAPWATCH_SEVERITY_CRITICAL},
    {KEY_OUT_DRAIN_MS, "Outbox drain timeout",
     "Maximum time to drain the MQTT outbox before terminal offline actions.", "int", false,
     true, "ms", static_cast<int32_t>(MIN_OUTBOX_DRAIN_MS), static_cast<int32_t>(MAX_OUTBOX_DRAIN_MS)},
    {KEY_TENANT_SLUG, "Workspace slug",
     "Heapwatch workspace identifier for MQTT and API routing.", "string", false, false, nullptr, 0,
     0},
    {KEY_DEVICE_UID, "Device UID", "Stable device identifier within the workspace.", "string",
     false, false, nullptr, 0, 0},
    {KEY_MQTT_PASSWORD, "MQTT password", "Broker authentication secret for this device.", "string",
     true, false, nullptr, 0, 0},
    {KEY_DEVICE_API_KEY, "Device API key",
     "Bearer token for device-authenticated REST ingress.", "string", true, false, nullptr, 0, 0},
    {KEY_STREAM_API_KEY, "Stream API key", "Separate secret for high-rate stream ingress.",
     "string", true, false, nullptr, 0, 0},
    {KEY_PENDING_UID, "Pending enrollment UID",
     "Temporary device UID while enrollment is in progress.", "string", false, false, nullptr, 0,
     0},
    {KEY_PROVISIONED, "Provisioned", "Whether device credentials have been committed to NVS.",
     "bool", false, false, nullptr, 0, 0},
    {KEY_BOOT_CYCLE, "Boot cycle", "Monotonic boot counter persisted across reboots.", "int",
     false, false, nullptr, 0, 0},
    {KEY_LAST_CRASH_BC, "Last crash boot cycle",
     "Boot cycle recorded when the previous crash occurred.", "int", false, false, nullptr, 0, 0},
};

struct ManagedCatalog {
    const char* id;
    const char* title;
    const char* nvs_namespace;
    FieldSchema fields[MAX_CATALOG_FIELDS] = {};
    size_t field_count;
};

struct CachedSettings {
    uint32_t telemetry_interval_ms = HEAPWATCH_METRICS_INTERVAL_MS;
    uint8_t min_relay_severity = HEAPWATCH_MIN_RELAY_SEVERITY;
    uint32_t outbox_drain_timeout_ms = DEFAULT_OUTBOX_DRAIN_MS;
};

ManagedCatalog g_catalogs[MAX_MANAGED_CATALOGS] = {};
size_t g_catalog_count = 0;

CachedSettings g_cache = {};
CachedSettings g_persisted = {};

struct OverlayEntry {
    char nvs_namespace[16];
    char nvs_key[16];
    cJSON* value;
};

constexpr size_t MAX_OVERLAY_ENTRIES = 16;
OverlayEntry g_overlay[MAX_OVERLAY_ENTRIES] = {};

bool overlayKeyEquals(const OverlayEntry& entry, const char* nvs_namespace, const char* nvs_key) {
    return entry.value != nullptr && strcmp(entry.nvs_namespace, nvs_namespace) == 0 &&
           strcmp(entry.nvs_key, nvs_key) == 0;
}

cJSON* findOverlayValue(const char* nvs_namespace, const char* nvs_key) {
    for (size_t i = 0; i < MAX_OVERLAY_ENTRIES; ++i) {
        if (overlayKeyEquals(g_overlay[i], nvs_namespace, nvs_key)) {
            return g_overlay[i].value;
        }
    }
    return nullptr;
}

void clearOverlay(const char* nvs_namespace, const char* nvs_key) {
    for (size_t i = 0; i < MAX_OVERLAY_ENTRIES; ++i) {
        if (overlayKeyEquals(g_overlay[i], nvs_namespace, nvs_key)) {
            cJSON_Delete(g_overlay[i].value);
            g_overlay[i].value = nullptr;
            g_overlay[i].nvs_namespace[0] = '\0';
            g_overlay[i].nvs_key[0] = '\0';
            return;
        }
    }
}

bool setOverlay(const char* nvs_namespace, const char* nvs_key, const cJSON* value) {
    OverlayEntry* slot = nullptr;
    for (size_t i = 0; i < MAX_OVERLAY_ENTRIES; ++i) {
        if (overlayKeyEquals(g_overlay[i], nvs_namespace, nvs_key)) {
            slot = &g_overlay[i];
            break;
        }
        if (slot == nullptr && g_overlay[i].value == nullptr) {
            slot = &g_overlay[i];
        }
    }
    if (slot == nullptr) {
        return false;
    }

    cJSON* copy = cJSON_Duplicate(value, true);
    if (copy == nullptr) {
        return false;
    }

    if (slot->value != nullptr) {
        cJSON_Delete(slot->value);
    }
    snprintf(slot->nvs_namespace, sizeof(slot->nvs_namespace), "%s", nvs_namespace);
    snprintf(slot->nvs_key, sizeof(slot->nvs_key), "%s", nvs_key);
    slot->value = copy;
    return true;
}

const ManagedCatalog* findCatalog(const char* nvs_namespace) {
    if (nvs_namespace == nullptr) {
        return nullptr;
    }
    for (size_t i = 0; i < g_catalog_count; ++i) {
        if (strcmp(g_catalogs[i].nvs_namespace, nvs_namespace) == 0) {
            return &g_catalogs[i];
        }
    }
    return nullptr;
}

const FieldSchema* findField(const ManagedCatalog* catalog, const char* nvs_key) {
    if (catalog == nullptr || nvs_key == nullptr) {
        return nullptr;
    }
    for (size_t i = 0; i < catalog->field_count; ++i) {
        if (strcmp(catalog->fields[i].nvs_key, nvs_key) == 0) {
            return &catalog->fields[i];
        }
    }
    return nullptr;
}

const FieldSchema* findCatalogField(const char* nvs_namespace, const char* nvs_key) {
    return findField(findCatalog(nvs_namespace), nvs_key);
}

bool readU32(nvs_handle_t handle, const char* key, uint32_t* out, uint32_t fallback) {
    uint32_t value = 0;
    const esp_err_t err = nvs_get_u32(handle, key, &value);
    if (err == ESP_OK) {
        *out = value;
        return true;
    }
    *out = fallback;
    return false;
}

bool readU8(nvs_handle_t handle, const char* key, uint8_t* out, uint8_t fallback) {
    uint8_t value = 0;
    const esp_err_t err = nvs_get_u8(handle, key, &value);
    if (err == ESP_OK) {
        *out = value;
        return true;
    }
    *out = fallback;
    return false;
}

void applyRuntime() {
    esp_log_level_t level = ESP_LOG_INFO;
    switch (g_cache.min_relay_severity) {
        case HEAPWATCH_SEVERITY_DEBUG:
            level = ESP_LOG_DEBUG;
            break;
        case HEAPWATCH_SEVERITY_INFO:
            level = ESP_LOG_INFO;
            break;
        case HEAPWATCH_SEVERITY_WARNING:
            level = ESP_LOG_WARN;
            break;
        case HEAPWATCH_SEVERITY_ERROR:
            level = ESP_LOG_ERROR;
            break;
        case HEAPWATCH_SEVERITY_CRITICAL:
            level = ESP_LOG_ERROR;
            break;
        default:
            level = ESP_LOG_INFO;
            break;
    }
    esp_log_level_set("*", level);
}

void loadSettingsFromNvs(CachedSettings* target) {
    nvs_handle_t handle = 0;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        target->telemetry_interval_ms = HEAPWATCH_METRICS_INTERVAL_MS;
        target->min_relay_severity = HEAPWATCH_MIN_RELAY_SEVERITY;
        target->outbox_drain_timeout_ms = DEFAULT_OUTBOX_DRAIN_MS;
        return;
    }

    readU32(handle, KEY_TEL_INT_MS, &target->telemetry_interval_ms,
            HEAPWATCH_METRICS_INTERVAL_MS);
    readU8(handle, KEY_LOG_LVL, &target->min_relay_severity, HEAPWATCH_MIN_RELAY_SEVERITY);
    readU32(handle, KEY_OUT_DRAIN_MS, &target->outbox_drain_timeout_ms, DEFAULT_OUTBOX_DRAIN_MS);

    if (target->telemetry_interval_ms < MIN_TELEMETRY_MS ||
        target->telemetry_interval_ms > MAX_TELEMETRY_MS) {
        target->telemetry_interval_ms = HEAPWATCH_METRICS_INTERVAL_MS;
    }
    if (target->min_relay_severity > HEAPWATCH_SEVERITY_CRITICAL) {
        target->min_relay_severity = HEAPWATCH_MIN_RELAY_SEVERITY;
    }
    if (target->outbox_drain_timeout_ms < MIN_OUTBOX_DRAIN_MS ||
        target->outbox_drain_timeout_ms > MAX_OUTBOX_DRAIN_MS) {
        target->outbox_drain_timeout_ms = DEFAULT_OUTBOX_DRAIN_MS;
    }

    nvs_close(handle);
}

void reloadFromNvs() {
    loadSettingsFromNvs(&g_cache);
    loadSettingsFromNvs(&g_persisted);
    applyRuntime();
}

bool writeU32(nvs_handle_t handle, const char* key, uint32_t value) {
    return nvs_set_u32(handle, key, value) == ESP_OK;
}

bool writeU8(nvs_handle_t handle, const char* key, uint8_t value) {
    return nvs_set_u8(handle, key, value) == ESP_OK;
}

bool validateIntField(const FieldSchema& field, const cJSON* value, int32_t* parsed) {
    if (!cJSON_IsNumber(value)) {
        return false;
    }
    const int32_t as_int = static_cast<int32_t>(value->valuedouble);
    if (as_int < field.min_value || as_int > field.max_value) {
        return false;
    }
    *parsed = as_int;
    return true;
}

bool jsonMatchesFieldType(const FieldSchema& field, const cJSON* value) {
    if (strcmp(field.type, "bool") == 0) {
        return cJSON_IsBool(value);
    }
    if (strcmp(field.type, "string") == 0) {
        return cJSON_IsString(value) && value->valuestring != nullptr;
    }
    if (strcmp(field.type, "int") == 0 || strcmp(field.type, "enum") == 0) {
        return cJSON_IsNumber(value);
    }
    return false;
}

cJSON* readNvsValueAsJson(nvs_handle_t handle, const char* key, nvs_type_t type) {
    if (type == NVS_TYPE_U8) {
        uint8_t v = 0;
        if (nvs_get_u8(handle, key, &v) != ESP_OK) {
            return nullptr;
        }
        return cJSON_CreateNumber(static_cast<double>(v));
    }
    if (type == NVS_TYPE_U32) {
        uint32_t v = 0;
        if (nvs_get_u32(handle, key, &v) != ESP_OK) {
            return nullptr;
        }
        return cJSON_CreateNumber(static_cast<double>(v));
    }
    if (type == NVS_TYPE_I32) {
        int32_t v = 0;
        if (nvs_get_i32(handle, key, &v) != ESP_OK) {
            return nullptr;
        }
        return cJSON_CreateNumber(static_cast<double>(v));
    }
    if (type == NVS_TYPE_STR) {
        char buf[128] = {};
        size_t len = sizeof(buf);
        if (nvs_get_str(handle, key, buf, &len) != ESP_OK) {
            return nullptr;
        }
        return cJSON_CreateString(buf);
    }
    return nullptr;
}

void addEnumOptions(cJSON* field_obj, const char* nvs_key) {
    if (strcmp(nvs_key, KEY_LOG_LVL) != 0) {
        return;
    }
    cJSON* options = cJSON_CreateArray();
    cJSON_AddItemToObject(field_obj, "options", options);
    struct Option {
        int32_t value;
        const char* label;
    };
    static const Option LOG_OPTIONS[] = {
        {HEAPWATCH_SEVERITY_DEBUG, "Debug"},
        {HEAPWATCH_SEVERITY_INFO, "Info"},
        {HEAPWATCH_SEVERITY_WARNING, "Warning"},
        {HEAPWATCH_SEVERITY_ERROR, "Error"},
        {HEAPWATCH_SEVERITY_CRITICAL, "Critical"},
    };
    for (const auto& opt : LOG_OPTIONS) {
        cJSON* option = cJSON_CreateObject();
        char value_buf[16];
        std::snprintf(value_buf, sizeof(value_buf), "%" PRId32, opt.value);
        cJSON_AddStringToObject(option, "value", value_buf);
        cJSON_AddStringToObject(option, "label", opt.label);
        cJSON_AddItemToArray(options, option);
    }
}

void addCachedFieldToJson(cJSON* values, const char* nvs_key, const CachedSettings& settings) {
    if (strcmp(nvs_key, KEY_LOG_LVL) == 0) {
        cJSON_AddNumberToObject(values, nvs_key, static_cast<double>(settings.min_relay_severity));
        return;
    }
    if (strcmp(nvs_key, KEY_TEL_INT_MS) == 0) {
        cJSON_AddNumberToObject(values, nvs_key,
                                static_cast<double>(settings.telemetry_interval_ms));
        return;
    }
    if (strcmp(nvs_key, KEY_OUT_DRAIN_MS) == 0) {
        cJSON_AddNumberToObject(values, nvs_key,
                                static_cast<double>(settings.outbox_drain_timeout_ms));
    }
}

bool isCachedPatchableKey(const char* nvs_key) {
    return strcmp(nvs_key, KEY_LOG_LVL) == 0 || strcmp(nvs_key, KEY_TEL_INT_MS) == 0 ||
           strcmp(nvs_key, KEY_OUT_DRAIN_MS) == 0;
}

cJSON* readCatalogFieldFromNvs(nvs_handle_t handle, const FieldSchema& field) {
    if (strcmp(field.type, "bool") == 0) {
        return readNvsValueAsJson(handle, field.nvs_key, NVS_TYPE_U8);
    }
    if (strcmp(field.type, "int") == 0 || strcmp(field.type, "enum") == 0) {
        cJSON* value = readNvsValueAsJson(handle, field.nvs_key, NVS_TYPE_U32);
        if (value != nullptr) {
            return value;
        }
        return readNvsValueAsJson(handle, field.nvs_key, NVS_TYPE_U8);
    }
    if (strcmp(field.type, "string") == 0) {
        return readNvsValueAsJson(handle, field.nvs_key, NVS_TYPE_STR);
    }
    return nullptr;
}

void addCatalogFieldToJson(cJSON* values, nvs_handle_t handle, const FieldSchema& field,
                           const CachedSettings& settings, const char* nvs_namespace) {
    if (field.secret) {
        cJSON_AddStringToObject(values, field.nvs_key, MASKED_SECRET);
        return;
    }

    cJSON* overlay = findOverlayValue(nvs_namespace, field.nvs_key);
    if (overlay != nullptr) {
        cJSON_AddItemToObject(values, field.nvs_key, cJSON_Duplicate(overlay, true));
        return;
    }

    if (isCachedPatchableKey(field.nvs_key)) {
        addCachedFieldToJson(values, field.nvs_key, settings);
        return;
    }

    cJSON* value = readCatalogFieldFromNvs(handle, field);
    if (value != nullptr) {
        cJSON_AddItemToObject(values, field.nvs_key, value);
    }
}

bool applyParsedToCache(const char* nvs_key, int32_t parsed) {
    if (strcmp(nvs_key, KEY_LOG_LVL) == 0) {
        g_cache.min_relay_severity = static_cast<uint8_t>(parsed);
        return true;
    }
    if (strcmp(nvs_key, KEY_TEL_INT_MS) == 0) {
        g_cache.telemetry_interval_ms = static_cast<uint32_t>(parsed);
        return true;
    }
    if (strcmp(nvs_key, KEY_OUT_DRAIN_MS) == 0) {
        g_cache.outbox_drain_timeout_ms = static_cast<uint32_t>(parsed);
        return true;
    }
    return false;
}

bool persistKeyToNvs(const char* nvs_key) {
    nvs_handle_t handle = 0;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        return false;
    }

    bool ok = false;
    if (strcmp(nvs_key, KEY_LOG_LVL) == 0) {
        ok = writeU8(handle, nvs_key, g_cache.min_relay_severity);
    } else if (strcmp(nvs_key, KEY_TEL_INT_MS) == 0) {
        ok = writeU32(handle, nvs_key, g_cache.telemetry_interval_ms);
    } else if (strcmp(nvs_key, KEY_OUT_DRAIN_MS) == 0) {
        ok = writeU32(handle, nvs_key, g_cache.outbox_drain_timeout_ms);
    }

    if (ok) {
        ok = nvs_commit(handle) == ESP_OK;
    }
    nvs_close(handle);
    return ok;
}

bool isKeyDirty(const char* nvs_namespace, const char* nvs_key) {
    if (findOverlayValue(nvs_namespace, nvs_key) != nullptr) {
        return true;
    }
    if (strcmp(nvs_key, KEY_LOG_LVL) == 0) {
        return g_cache.min_relay_severity != g_persisted.min_relay_severity;
    }
    if (strcmp(nvs_key, KEY_TEL_INT_MS) == 0) {
        return g_cache.telemetry_interval_ms != g_persisted.telemetry_interval_ms;
    }
    if (strcmp(nvs_key, KEY_OUT_DRAIN_MS) == 0) {
        return g_cache.outbox_drain_timeout_ms != g_persisted.outbox_drain_timeout_ms;
    }
    return false;
}

void copyPersistedKeyToCache(const char* nvs_namespace, const char* nvs_key) {
    clearOverlay(nvs_namespace, nvs_key);
    if (strcmp(nvs_key, KEY_LOG_LVL) == 0) {
        g_cache.min_relay_severity = g_persisted.min_relay_severity;
        return;
    }
    if (strcmp(nvs_key, KEY_TEL_INT_MS) == 0) {
        g_cache.telemetry_interval_ms = g_persisted.telemetry_interval_ms;
        return;
    }
    if (strcmp(nvs_key, KEY_OUT_DRAIN_MS) == 0) {
        g_cache.outbox_drain_timeout_ms = g_persisted.outbox_drain_timeout_ms;
    }
}

void copyCacheKeyToPersisted(const char* nvs_key) {
    if (strcmp(nvs_key, KEY_LOG_LVL) == 0) {
        g_persisted.min_relay_severity = g_cache.min_relay_severity;
        return;
    }
    if (strcmp(nvs_key, KEY_TEL_INT_MS) == 0) {
        g_persisted.telemetry_interval_ms = g_cache.telemetry_interval_ms;
        return;
    }
    if (strcmp(nvs_key, KEY_OUT_DRAIN_MS) == 0) {
        g_persisted.outbox_drain_timeout_ms = g_cache.outbox_drain_timeout_ms;
    }
}

cJSON* buildCatalogJson(const ManagedCatalog& catalog) {
    cJSON* catalog_obj = cJSON_CreateObject();
    cJSON_AddStringToObject(catalog_obj, "id", catalog.id);
    cJSON_AddStringToObject(catalog_obj, "title", catalog.title);
    cJSON_AddStringToObject(catalog_obj, "nvs_namespace", catalog.nvs_namespace);

    cJSON* fields = cJSON_CreateArray();
    cJSON_AddItemToObject(catalog_obj, "fields", fields);

    for (size_t i = 0; i < catalog.field_count; ++i) {
        const FieldSchema& field = catalog.fields[i];
        cJSON* field_obj = cJSON_CreateObject();
        cJSON_AddStringToObject(field_obj, "nvs_key", field.nvs_key);
        cJSON_AddStringToObject(field_obj, "name", field.name);
        cJSON_AddStringToObject(field_obj, "description", field.description);
        cJSON_AddStringToObject(field_obj, "type", field.type);
        cJSON_AddBoolToObject(field_obj, "secret", field.secret);
        cJSON_AddBoolToObject(field_obj, "patchable", field.patchable);
        if (field.unit != nullptr) {
            cJSON_AddStringToObject(field_obj, "unit", field.unit);
        }
        cJSON_AddNumberToObject(field_obj, "min", field.min_value);
        cJSON_AddNumberToObject(field_obj, "max", field.max_value);

        if (field.secret) {
            cJSON_AddStringToObject(field_obj, "default", "");
        } else if (strcmp(field.nvs_key, KEY_TEL_INT_MS) == 0) {
            cJSON_AddNumberToObject(field_obj, "default",
                                    static_cast<double>(HEAPWATCH_METRICS_INTERVAL_MS));
        } else if (strcmp(field.nvs_key, KEY_LOG_LVL) == 0) {
            cJSON_AddNumberToObject(field_obj, "default",
                                    static_cast<double>(HEAPWATCH_MIN_RELAY_SEVERITY));
        } else if (strcmp(field.nvs_key, KEY_OUT_DRAIN_MS) == 0) {
            cJSON_AddNumberToObject(field_obj, "default",
                                    static_cast<double>(DEFAULT_OUTBOX_DRAIN_MS));
        }

        addEnumOptions(field_obj, field.nvs_key);
        cJSON_AddItemToArray(fields, field_obj);
    }

    return catalog_obj;
}

void collectNamespaces(std::vector<std::string>& namespaces) {
    namespaces.clear();
    nvs_iterator_t it = nullptr;
    esp_err_t err = nvs_entry_find(NVS_DEFAULT_PART_NAME, nullptr, NVS_TYPE_ANY, &it);
    while (err == ESP_OK) {
        nvs_entry_info_t info = {};
        nvs_entry_info(it, &info);

        bool found = false;
        for (const auto& existing : namespaces) {
            if (existing == info.namespace_name) {
                found = true;
                break;
            }
        }
        if (!found) {
            namespaces.emplace_back(info.namespace_name);
        }

        err = nvs_entry_next(&it);
    }
    nvs_release_iterator(it);
}

cJSON* buildManagedValuesObject(const ManagedCatalog& catalog, const CachedSettings& settings) {
    cJSON* values = cJSON_CreateObject();

    nvs_handle_t handle = 0;
    const bool has_handle = nvs_open(catalog.nvs_namespace, NVS_READONLY, &handle) == ESP_OK;

    for (size_t i = 0; i < catalog.field_count; ++i) {
        addCatalogFieldToJson(values, has_handle ? handle : 0, catalog.fields[i], settings,
                              catalog.nvs_namespace);
    }

    if (has_handle) {
        nvs_close(handle);
    }

    return values;
}

bool writeJsonValueToNvs(nvs_handle_t handle, const char* key, const cJSON* value) {
    if (cJSON_IsBool(value)) {
        return writeU8(handle, key, cJSON_IsTrue(value) ? 1 : 0);
    }
    if (cJSON_IsNumber(value)) {
        const int32_t as_int = static_cast<int32_t>(value->valuedouble);
        if (writeU32(handle, key, static_cast<uint32_t>(as_int))) {
            return true;
        }
        return nvs_set_i32(handle, key, as_int) == ESP_OK;
    }
    if (cJSON_IsString(value) && value->valuestring != nullptr) {
        return nvs_set_str(handle, key, value->valuestring) == ESP_OK;
    }
    return false;
}

}  // namespace

void init() {
    registerManagedCatalog("heapwatch", "Heapwatch Connector", NVS_NAMESPACE, HEAPWATCH_FIELDS,
                           sizeof(HEAPWATCH_FIELDS) / sizeof(HEAPWATCH_FIELDS[0]));
    reloadFromNvs();
    ESP_LOGI(TAG, "telemetry=%" PRIu32 " ms log_lvl=%" PRIu32 " outbox_drain=%" PRIu32 " ms",
             static_cast<uint32_t>(g_cache.telemetry_interval_ms),
             static_cast<uint32_t>(g_cache.min_relay_severity),
             static_cast<uint32_t>(g_cache.outbox_drain_timeout_ms));
}

uint32_t telemetryIntervalMs() { return g_cache.telemetry_interval_ms; }

uint8_t minRelaySeverity() { return g_cache.min_relay_severity; }

uint32_t outboxDrainTimeoutMs() { return g_cache.outbox_drain_timeout_ms; }

bool isManagedNamespace(const char* nvs_namespace) {
    return findCatalog(nvs_namespace) != nullptr;
}

bool isPatchableKey(const char* nvs_key) {
    const FieldSchema* field = findCatalogField(NVS_NAMESPACE, nvs_key);
    return field != nullptr && field->patchable;
}

bool isSecretKey(const char* nvs_key) {
    const FieldSchema* field = findCatalogField(NVS_NAMESPACE, nvs_key);
    return field != nullptr && field->secret;
}

bool isInternalKey(const char* nvs_key) {
    const FieldSchema* field = findCatalogField(NVS_NAMESPACE, nvs_key);
    return field != nullptr && !field->patchable;
}

bool shouldMaskNvsKey(const char* nvs_namespace, const char* nvs_key) {
    const FieldSchema* field = findCatalogField(nvs_namespace, nvs_key);
    return field != nullptr && field->secret;
}

bool registerManagedCatalog(const char* catalog_id, const char* title, const char* nvs_namespace,
                            const FieldSchema* fields, size_t field_count) {
    if (catalog_id == nullptr || title == nullptr || nvs_namespace == nullptr || fields == nullptr ||
        field_count == 0 || field_count > MAX_CATALOG_FIELDS ||
        g_catalog_count >= MAX_MANAGED_CATALOGS) {
        return false;
    }
    if (findCatalog(nvs_namespace) != nullptr) {
        return false;
    }
    ManagedCatalog& catalog = g_catalogs[g_catalog_count];
    catalog.id = catalog_id;
    catalog.title = title;
    catalog.nvs_namespace = nvs_namespace;
    catalog.field_count = field_count;
    for (size_t i = 0; i < field_count; ++i) {
        catalog.fields[i] = fields[i];
    }
    ++g_catalog_count;
    return true;
}

bool setValue(const char* nvs_namespace, const char* nvs_key, const cJSON* value, char* error_out,
              size_t error_out_len) {
    const ManagedCatalog* catalog = findCatalog(nvs_namespace);
    if (catalog == nullptr) {
        if (error_out != nullptr && error_out_len > 0) {
            snprintf(error_out, error_out_len, "namespace not managed");
        }
        return false;
    }

    const FieldSchema* field = findField(catalog, nvs_key);
    if (field == nullptr || !field->patchable) {
        if (error_out != nullptr && error_out_len > 0) {
            snprintf(error_out, error_out_len, "key not settable");
        }
        return false;
    }

    if (!jsonMatchesFieldType(*field, value)) {
        if (error_out != nullptr && error_out_len > 0) {
            snprintf(error_out, error_out_len, "invalid value for %s", nvs_key);
        }
        return false;
    }

    if (strcmp(field->type, "int") == 0 || strcmp(field->type, "enum") == 0) {
        int32_t parsed = 0;
        if (!validateIntField(*field, value, &parsed)) {
            if (error_out != nullptr && error_out_len > 0) {
                snprintf(error_out, error_out_len, "invalid value for %s", nvs_key);
            }
            return false;
        }
        if (applyParsedToCache(nvs_key, parsed)) {
            applyRuntime();
            return true;
        }
    }

    if (!setOverlay(nvs_namespace, nvs_key, value)) {
        if (error_out != nullptr && error_out_len > 0) {
            snprintf(error_out, error_out_len, "unsupported key %s", nvs_key);
        }
        return false;
    }

    return true;
}

bool persistManagedValues(const char* nvs_namespace, const cJSON* keys_or_null, char* error_out,
                          size_t error_out_len) {
    const ManagedCatalog* catalog = findCatalog(nvs_namespace);
    if (catalog == nullptr) {
        if (error_out != nullptr && error_out_len > 0) {
            snprintf(error_out, error_out_len, "namespace not managed");
        }
        return false;
    }

    auto persistOneKey = [&](const char* key) -> bool {
        if (findField(catalog, key) == nullptr) {
            if (error_out != nullptr && error_out_len > 0) {
                snprintf(error_out, error_out_len, "unknown persist key");
            }
            return false;
        }
        if (isCachedPatchableKey(key) && persistKeyToNvs(key)) {
            copyCacheKeyToPersisted(key);
            return true;
        }

        cJSON* overlay = findOverlayValue(catalog->nvs_namespace, key);
        if (overlay == nullptr) {
            if (error_out != nullptr && error_out_len > 0) {
                snprintf(error_out, error_out_len, "nvs persist failed");
            }
            return false;
        }

        nvs_handle_t handle = 0;
        if (nvs_open(catalog->nvs_namespace, NVS_READWRITE, &handle) != ESP_OK) {
            if (error_out != nullptr && error_out_len > 0) {
                snprintf(error_out, error_out_len, "nvs persist failed");
            }
            return false;
        }
        bool ok = writeJsonValueToNvs(handle, key, overlay);
        if (ok) {
            ok = nvs_commit(handle) == ESP_OK;
        }
        nvs_close(handle);
        if (!ok) {
            if (error_out != nullptr && error_out_len > 0) {
                snprintf(error_out, error_out_len, "nvs persist failed");
            }
            return false;
        }
        clearOverlay(catalog->nvs_namespace, key);
        return true;
    };

    if (keys_or_null != nullptr && cJSON_IsObject(keys_or_null)) {
        const cJSON* child = keys_or_null->child;
        while (child != nullptr) {
            if (child->string == nullptr || !persistOneKey(child->string)) {
                return false;
            }
            child = child->next;
        }
        return true;
    }

    for (size_t i = 0; i < catalog->field_count; ++i) {
        const char* key = catalog->fields[i].nvs_key;
        if (!isKeyDirty(catalog->nvs_namespace, key)) {
            continue;
        }
        if (!persistOneKey(key)) {
            return false;
        }
    }

    return true;
}

bool revertManagedValues(const char* nvs_namespace, const cJSON* keys, char* error_out,
                         size_t error_out_len) {
    const ManagedCatalog* catalog = findCatalog(nvs_namespace);
    if (catalog == nullptr) {
        if (error_out != nullptr && error_out_len > 0) {
            snprintf(error_out, error_out_len, "namespace not managed");
        }
        return false;
    }

    if (keys != nullptr && cJSON_IsArray(keys)) {
        const int32_t count = cJSON_GetArraySize(keys);
        for (int32_t i = 0; i < count; ++i) {
            const cJSON* item = cJSON_GetArrayItem(keys, i);
            if (!cJSON_IsString(item) || item->valuestring == nullptr) {
                if (error_out != nullptr && error_out_len > 0) {
                    snprintf(error_out, error_out_len, "invalid revert key");
                }
                return false;
            }
            if (findField(catalog, item->valuestring) == nullptr) {
                if (error_out != nullptr && error_out_len > 0) {
                    snprintf(error_out, error_out_len, "unknown revert key");
                }
                return false;
            }
            copyPersistedKeyToCache(catalog->nvs_namespace, item->valuestring);
        }
        applyRuntime();
        return true;
    }

    for (size_t i = 0; i < catalog->field_count; ++i) {
        copyPersistedKeyToCache(catalog->nvs_namespace, catalog->fields[i].nvs_key);
    }
    applyRuntime();
    return true;
}

cJSON* buildManagedNamespaceResponse(const char* nvs_namespace) {
    const ManagedCatalog* catalog = findCatalog(nvs_namespace);
    if (catalog == nullptr) {
        return nullptr;
    }

    cJSON* result = cJSON_CreateObject();
    cJSON_AddItemToObject(result, "values", buildManagedValuesObject(*catalog, g_cache));
    cJSON_AddItemToObject(result, "persisted_values",
                          buildManagedValuesObject(*catalog, g_persisted));
    return result;
}

cJSON* buildUnmanagedNamespaceResponse(const char* nvs_namespace) {
    if (nvs_namespace == nullptr || isManagedNamespace(nvs_namespace)) {
        return nullptr;
    }

    cJSON* result = cJSON_CreateObject();
    cJSON* persisted = cJSON_CreateObject();
    cJSON_AddItemToObject(result, "persisted_values", persisted);

    nvs_handle_t handle = 0;
    if (nvs_open(nvs_namespace, NVS_READONLY, &handle) != ESP_OK) {
        return result;
    }

    nvs_iterator_t it = nullptr;
    esp_err_t err = nvs_entry_find(NVS_DEFAULT_PART_NAME, nvs_namespace, NVS_TYPE_ANY, &it);
    while (err == ESP_OK) {
        nvs_entry_info_t info = {};
        nvs_entry_info(it, &info);

        if (shouldMaskNvsKey(nvs_namespace, info.key)) {
            cJSON_AddStringToObject(persisted, info.key, MASKED_SECRET);
        } else {
            cJSON* value = readNvsValueAsJson(handle, info.key, info.type);
            if (value != nullptr) {
                cJSON_AddItemToObject(persisted, info.key, value);
            }
        }

        err = nvs_entry_next(&it);
    }
    nvs_release_iterator(it);
    nvs_close(handle);

    return result;
}

cJSON* buildFullPartitionResponse() {
    cJSON* result = cJSON_CreateObject();
    cJSON* managed = cJSON_CreateObject();
    cJSON* unmanaged = cJSON_CreateObject();
    cJSON_AddItemToObject(result, "managed", managed);
    cJSON_AddItemToObject(result, "unmanaged", unmanaged);

    std::vector<std::string> namespaces;
    collectNamespaces(namespaces);

    for (const auto& ns : namespaces) {
        if (isManagedNamespace(ns.c_str())) {
            cJSON* ns_obj = buildManagedNamespaceResponse(ns.c_str());
            if (ns_obj != nullptr) {
                cJSON_AddItemToObject(managed, ns.c_str(), ns_obj);
            }
            continue;
        }

        cJSON* ns_obj = buildUnmanagedNamespaceResponse(ns.c_str());
        if (ns_obj != nullptr) {
            cJSON_AddItemToObject(unmanaged, ns.c_str(), ns_obj);
        }
    }

    return result;
}

bool persistUnmanagedValues(const char* nvs_namespace, const cJSON* values, char* error_out,
                            size_t error_out_len) {
    if (nvs_namespace == nullptr || values == nullptr || !cJSON_IsObject(values)) {
        if (error_out != nullptr && error_out_len > 0) {
            snprintf(error_out, error_out_len, "invalid persist payload");
        }
        return false;
    }

    if (isManagedNamespace(nvs_namespace)) {
        return persistManagedValues(nvs_namespace, values, error_out, error_out_len);
    }

    nvs_handle_t handle = 0;
    if (nvs_open(nvs_namespace, NVS_READWRITE, &handle) != ESP_OK) {
        if (error_out != nullptr && error_out_len > 0) {
            snprintf(error_out, error_out_len, "nvs open failed");
        }
        return false;
    }

    const cJSON* child = values->child;
    while (child != nullptr) {
        if (child->string == nullptr) {
            nvs_close(handle);
            if (error_out != nullptr && error_out_len > 0) {
                snprintf(error_out, error_out_len, "invalid values key");
            }
            return false;
        }
        const FieldSchema* field = findCatalogField(nvs_namespace, child->string);
        if (field != nullptr && (!field->patchable || field->secret)) {
            nvs_close(handle);
            if (error_out != nullptr && error_out_len > 0) {
                snprintf(error_out, error_out_len, "key not persistable");
            }
            return false;
        }
        if (!writeJsonValueToNvs(handle, child->string, child)) {
            nvs_close(handle);
            if (error_out != nullptr && error_out_len > 0) {
                snprintf(error_out, error_out_len, "nvs write failed");
            }
            return false;
        }
        child = child->next;
    }

    const bool ok = nvs_commit(handle) == ESP_OK;
    nvs_close(handle);
    if (!ok && error_out != nullptr && error_out_len > 0) {
        snprintf(error_out, error_out_len, "nvs commit failed");
    }
    return ok;
}

cJSON* buildAllConfigCatalogsJson() {
    cJSON* catalogs = cJSON_CreateArray();
    for (size_t i = 0; i < g_catalog_count; ++i) {
        cJSON_AddItemToArray(catalogs, buildCatalogJson(g_catalogs[i]));
    }
    return catalogs;
}

cJSON* buildUserConfigCatalogsJson() {
    cJSON* catalogs = cJSON_CreateArray();
    for (size_t i = 1; i < g_catalog_count; ++i) {
        cJSON_AddItemToArray(catalogs, buildCatalogJson(g_catalogs[i]));
    }
    return catalogs;
}

bool hasUserConfigCatalogs() { return g_catalog_count > 1; }

}  // namespace Heapwatch::Settings
