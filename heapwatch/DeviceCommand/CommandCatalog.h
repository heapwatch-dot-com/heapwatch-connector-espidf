#pragma once

namespace Heapwatch::DeviceCommand {

/** Flash-resident builtin command catalog JSON (commands[] only). */
const char* getBuiltinCommandCatalogJson();

/** Stream command catalog over mux channel command_catalog once per boot after MQTT connects. */
void uploadCommandCatalogIfNeeded();

/** Stream command catalog (respects boot flag unless force). */
void uploadCommandCatalog(bool force);

/** Non-blocking request for hw_streaming to upload the command catalog. */
void requestCommandCatalogUpload(bool force = false);

/** True after a successful command catalog stream upload this boot. */
bool commandCatalogUploadedThisBoot();

/** Pop a pending upload request (called from hw_streaming). Returns false when none pending. */
bool consumeCommandCatalogUploadRequest(bool* force_out);

void resetCommandCatalogBootFlags();

}  // namespace Heapwatch::DeviceCommand
