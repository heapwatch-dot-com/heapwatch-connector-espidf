#pragma once

struct cJSON;

namespace heapwatch::device_command {

/** Flash-resident builtin manifest JSON (see CommandManifestBuiltin.h). */
const char* getBuiltinCommandManifestJson();

/** Stream command manifest v2 over mux channel command_manifest once per boot after MQTT connects. */
void uploadManifestIfNeeded();

/** Parse builtin manifest for manifest/get (caller owns returned cJSON*). */
cJSON* buildManifestRootObject();

}  // namespace heapwatch::device_command
