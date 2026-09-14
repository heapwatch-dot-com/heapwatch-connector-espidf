#pragma once

#include <cstddef>
#include <cstdint>

#include "cJSON.h"

namespace heapwatch::connector_settings {

struct FieldSchema {
    const char* nvs_key;
    const char* name;
    const char* description;
    const char* type;
    bool secret;
    bool patchable;
    const char* unit;
    int32_t min_value;
    int32_t max_value;
};

/** Load NVS-backed connector settings and apply runtime defaults. Call after nvs_flash_init. */
void init();

uint32_t telemetryIntervalMs();
uint8_t minRelaySeverity();
uint32_t outboxDrainTimeoutMs();

bool isManagedNamespace(const char* nvs_namespace);
bool isPatchableKey(const char* nvs_key);
bool isSecretKey(const char* nvs_key);
bool isInternalKey(const char* nvs_key);
bool shouldMaskNvsKey(const char* nvs_namespace, const char* nvs_key);

/**
 * Register a managed config catalog for an NVS namespace. The namespace becomes managed
 * and participates in the two-step set / persist / revert flow.
 */
bool registerManagedCatalog(const char* catalog_id, const char* title, const char* nvs_namespace,
                            const FieldSchema* fields, size_t field_count);

/** Update in-RAM cache for a managed namespace key. Does not write NVS. */
bool setValue(const char* nvs_namespace, const char* nvs_key, const cJSON* value, char* error_out,
              size_t error_out_len);

/** Persist managed keys from cache to NVS. values=null persists all dirty keys. */
bool persistManagedValues(const char* nvs_namespace, const cJSON* values, char* error_out,
                          size_t error_out_len);

/** Revert managed cache from persisted NVS snapshot. values=null reverts all dirty keys. */
bool revertManagedValues(const char* nvs_namespace, const cJSON* keys, char* error_out,
                         size_t error_out_len);

/** Managed namespace: { values, persisted_values }. */
cJSON* buildManagedNamespaceResponse(const char* nvs_namespace);

/** Unmanaged namespace: { persisted_values } with secrets masked. */
cJSON* buildUnmanagedNamespaceResponse(const char* nvs_namespace);

/** Full partition: { managed: { ns: ... }, unmanaged: { ns: ... } }. */
cJSON* buildFullPartitionResponse();

/** Direct NVS write for unmanaged namespaces. */
bool persistUnmanagedValues(const char* nvs_namespace, const cJSON* values, char* error_out,
                            size_t error_out_len);

/** All registered catalogs for config catalog upload. */
cJSON* buildAllConfigCatalogsJson();

/** User-registered catalogs only (excludes builtin heapwatch catalog at index 0). */
cJSON* buildUserConfigCatalogsJson();

/** True when the client app registered at least one config catalog via heapwatch_register_config_catalog. */
bool hasUserConfigCatalogs();

}  // namespace heapwatch::connector_settings
