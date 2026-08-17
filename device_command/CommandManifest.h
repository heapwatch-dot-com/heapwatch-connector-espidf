#pragma once

struct cJSON;

namespace heapwatch::device_command {

/** Stream command manifest v2 over mux channel command_manifest once per boot after MQTT connects. */
void uploadManifestIfNeeded();

/** Build manifest v2 root object for manifest/get (caller owns returned cJSON*). */
cJSON* buildManifestRootObject();

}  // namespace heapwatch::device_command
