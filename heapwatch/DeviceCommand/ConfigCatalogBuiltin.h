#pragma once

#include "heapwatch/DeviceCommand/CommandCatalogHeapwatchCatalog.inc.h"

namespace Heapwatch::DeviceCommand {

// Builtin config catalog document — catalogs[] only (no command catalog fields).
static const char BUILTIN_CONFIG_CATALOG_JSON[] =
    "{\"catalogs\":" HEAPWATCH_BUILTIN_CONFIG_CATALOGS_JSON "}";

}  // namespace Heapwatch::DeviceCommand
