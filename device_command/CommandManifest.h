#pragma once

struct cJSON;

namespace heapwatch::device_command {

/** Flash-resident builtin manifest JSON (see CommandManifestBuiltin.h). */
const char* getBuiltinCommandManifestJson();

/** Stream command manifest v2 over mux channel command_manifest once per boot after MQTT connects. */
void uploadManifestIfNeeded();

/** Non-blocking request for hw_streaming to upload the manifest when TCP is ready. */
void requestManifestUpload();

/** True after a successful manifest stream upload this boot. */
bool manifestUploadedThisBoot();

/** Parse builtin manifest for manifest/get (caller owns returned cJSON*). */
cJSON* buildManifestRootObject();

}  // namespace heapwatch::device_command
