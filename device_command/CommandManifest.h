#pragma once

namespace heapwatch::device_command {

/** POST manifest v2 to /ingress/command-manifest once per boot after MQTT connects. */
void uploadManifestIfNeeded();

}  // namespace heapwatch::device_command
