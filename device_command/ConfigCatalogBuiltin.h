#pragma once

#include "CommandCatalogHeapwatchCatalog.inc.h"

namespace heapwatch::device_command {

// Builtin config catalog document — catalogs[] only (no command catalog fields).
static const char BUILTIN_CONFIG_CATALOG_JSON[] =
    "{\"catalogs\":" HEAPWATCH_BUILTIN_CONFIG_CATALOGS_JSON "}";

}  // namespace heapwatch::device_command
