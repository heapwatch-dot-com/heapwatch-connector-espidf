#pragma once

#include <cstddef>

namespace Heapwatch::DeviceCommand {

/** Stream config snapshot JSON over mux channel config_snapshot (namespace "*" = full partition). */
void uploadConfigSnapshot(const char* nvs_namespace, bool force);

/** Non-blocking request for hw_streaming to upload a config snapshot. */
void requestConfigSnapshotUpload(const char* nvs_namespace, bool force = false);

/** True after a successful full-partition snapshot stream upload this boot (unless force). */
bool configSnapshotUploadedThisBoot();

/** Pop a pending upload request (called from hw_streaming). Returns false when none pending. */
bool consumeConfigSnapshotUploadRequest(char* namespace_out, size_t namespace_out_len, bool* force_out);

void resetConfigSnapshotBootFlags();

}  // namespace Heapwatch::DeviceCommand
