#pragma once

namespace heapwatch::device_command {

/** Stream config catalog JSON over mux channel config_catalog. */
void uploadConfigCatalog(bool force);

/** Non-blocking request for hw_streaming to upload the config catalog. */
void requestConfigCatalogUpload(bool force = false);

/** True after a successful config catalog stream upload this boot (unless force). */
bool configCatalogUploadedThisBoot();

/** Pop a pending upload request (called from hw_streaming). Returns false when none pending. */
bool consumeConfigCatalogUploadRequest(bool* force_out);

}  // namespace heapwatch::device_command
