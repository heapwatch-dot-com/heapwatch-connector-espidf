#pragma once

namespace heapwatch::device_command {

/** Stream command manifest v2 over mux channel command_manifest once per boot after MQTT connects. */
void uploadManifestIfNeeded();

}  // namespace heapwatch::device_command
