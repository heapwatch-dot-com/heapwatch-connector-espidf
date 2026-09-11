#pragma once

#include "esp_err.h"
#include "heapwatch.h"
#include "heapwatch_connector/ConnectorBootstrap.h"

namespace heapwatch::connector_lifecycle {

void setPhase(heapwatch_phase_t phase);
heapwatch_phase_t getPhase();

/** Idempotent. Returns immediately; bootstrap runs on a background task. */
esp_err_t initialize();

/** True after initialize() until deinitialize() completes. */
bool initializeCalled();

/** Stop all connector tasks and hooks so initialize() can be called again. */
void deinitialize();

bool activateConnectorFromCredentials(const connector_bootstrap::BootCredentials& creds);

}  // namespace heapwatch::connector_lifecycle
