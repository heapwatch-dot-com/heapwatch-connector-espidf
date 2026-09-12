#pragma once

#include "../heapwatch_config.h"

#define HW_CATALOG_INT(n) HW_CATALOG_INT2(n)
#define HW_CATALOG_INT2(n) #n

// Builtin heapwatch namespace config catalog — keep in sync with kHeapwatchFields in ConnectorSettings.cpp.
#define HEAPWATCH_BUILTIN_CONFIG_CATALOGS_JSON                                 \
    "[{\"id\":\"heapwatch\",\"title\":\"Heapwatch Connector\",\"nvs_namespace\":" \
    "\"heapwatch\",\"fields\":["                                               \
    "{\"nvs_key\":\"tel_int_ms\",\"name\":\"Telemetry "                        \
    "interval\",\"description\":\"Interval between automatic telemetry metric " \
    "collection cycles.\",\"type\":\"int\",\"secret\":false,\"patchable\":true," \
    "\"unit\":\"ms\",\"min\":1000,\"max\":86400000,\"default\":" HW_CATALOG_INT( \
        HEAPWATCH_METRICS_INTERVAL_MS) "},"                                     \
    "{\"nvs_key\":\"task_smp_ms\",\"name\":\"Task stack sampling "               \
    "interval\",\"description\":\"How often each task's stack pointer is "       \
    "sampled between telemetry cycles. Task stack Max only moves when a sample " \
    "is taken, so shorter intervals catch shorter bursts. Clamped to the "       \
    "telemetry interval.\",\"type\":\"int\",\"secret\":false,\"patchable\":true," \
    "\"unit\":\"ms\",\"min\":100,\"max\":5000,\"default\":" HW_CATALOG_INT(      \
        HEAPWATCH_TASK_SAMPLE_INTERVAL_MS) "},"                                  \
    "{\"nvs_key\":\"log_lvl\",\"name\":\"Log relay "                            \
    "level\",\"description\":\"Minimum ESP-IDF log severity relayed to "       \
    "Heapwatch over MQTT.\",\"type\":\"enum\",\"secret\":false,\"patchable\":"  \
    "true,\"min\":0,\"max\":4,\"default\":" HW_CATALOG_INT(                    \
        HEAPWATCH_MIN_RELAY_SEVERITY) ",\"options\":[{\"value\":\"0\",\"label\":\"Debug\"},{\"value\":\"1\","      \
    "\"label\":\"Info\"},{\"value\":\"2\",\"label\":\"Warning\"},{\"value\":"  \
    "\"3\",\"label\":\"Error\"},{\"value\":\"4\",\"label\":\"Critical\"}]},"   \
    "{\"nvs_key\":\"out_drain_ms\",\"name\":\"Outbox drain "                    \
    "timeout\",\"description\":\"Maximum time to drain the MQTT outbox before " \
    "terminal offline actions.\",\"type\":\"int\",\"secret\":false,"             \
    "\"patchable\":true,\"unit\":\"ms\",\"min\":1000,\"max\":120000,"           \
    "\"default\":30000},"                                                       \
    "{\"nvs_key\":\"tenant_slug\",\"name\":\"Workspace "                        \
    "slug\",\"description\":\"Heapwatch workspace identifier for MQTT and API " \
    "routing.\",\"type\":\"string\",\"secret\":false,\"patchable\":false},"     \
    "{\"nvs_key\":\"device_uid\",\"name\":\"Device "                            \
    "UID\",\"description\":\"Stable device identifier within the "              \
    "workspace.\",\"type\":\"string\",\"secret\":false,\"patchable\":false},"   \
    "{\"nvs_key\":\"mqtt_password\",\"name\":\"MQTT "                           \
    "password\",\"description\":\"Broker authentication secret for this "       \
    "device.\",\"type\":\"string\",\"secret\":true,\"patchable\":false,"        \
    "\"default\":\"\"},"                                                        \
    "{\"nvs_key\":\"device_api_key\",\"name\":\"Device API "                     \
    "key\",\"description\":\"Bearer token for device-authenticated REST "       \
    "ingress.\",\"type\":\"string\",\"secret\":true,\"patchable\":false,"       \
    "\"default\":\"\"},"                                                        \
    "{\"nvs_key\":\"stream_api_key\",\"name\":\"Stream API "                    \
    "key\",\"description\":\"Separate secret for high-rate stream "              \
    "ingress.\",\"type\":\"string\",\"secret\":true,\"patchable\":false,"       \
    "\"default\":\"\"},"                                                        \
    "{\"nvs_key\":\"pending_uid\",\"name\":\"Pending enrollment "                \
    "UID\",\"description\":\"Temporary device UID while enrollment is in "      \
    "progress.\",\"type\":\"string\",\"secret\":false,\"patchable\":false},"    \
    "{\"nvs_key\":\"provisioned\",\"name\":\"Provisioned\","                     \
    "\"description\":\"Whether device credentials have been committed to "       \
    "NVS.\",\"type\":\"bool\",\"secret\":false,\"patchable\":false},"           \
    "{\"nvs_key\":\"boot_cycle\",\"name\":\"Boot "                               \
    "cycle\",\"description\":\"Monotonic boot counter persisted across "         \
    "reboots.\",\"type\":\"int\",\"secret\":false,\"patchable\":false},"        \
    "{\"nvs_key\":\"last_crash_bc\",\"name\":\"Last crash boot "                \
    "cycle\",\"description\":\"Boot cycle recorded when the previous crash "     \
    "occurred.\",\"type\":\"int\",\"secret\":false,\"patchable\":false},"       \
    "{\"nvs_key\":\"man_hash\",\"name\":\"Command catalog "                            \
    "hash\",\"description\":\"SHA-256 of the last uploaded command catalog "    \
    "body.\",\"type\":\"string\",\"secret\":false,\"patchable\":false},"         \
    "{\"nvs_key\":\"man_build\",\"name\":\"Command catalog build "                     \
    "identifier\",\"description\":\"Build identifier of the last uploaded "      \
    "command catalog.\",\"type\":\"string\",\"secret\":false,\"patchable\":"   \
    "false}"                                                                    \
    "]}]"
