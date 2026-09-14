#pragma once

#include <cstddef>
#include <cstdint>

#include "cJSON.h"
#include "esp_err.h"

namespace heapwatch::device_command {

/**
 * Handle an inbound MQTT command message on {tenant}/command/device/{uid}/{suffix}.
 * Always publishes a response on the matching response topic when possible.
 *
 * mqtt_retain: MQTT retain flag from the broker (retained messages are ignored).
 */
void handleMqttMessage(const char* topic, size_t topic_len, const char* payload,
                        size_t payload_len, int32_t mqtt_retain);

/**
 * CMD1: client-defined custom inbound remote command handler.
 *
 * Called with the command suffix (matches the registered name) and the
 * command_id extracted from the inbound JSON payload (empty string if the
 * payload didn't include one — never nullptr). The handler owns responding
 * via publishCommandResponse() below; handleMqttMessage() does not publish
 * anything on the handler's behalf, since only the handler knows the real
 * outcome (ok/code/message).
 */
using CommandHandler = void (*)(const char* suffix, const char* command_id, void* user_ctx);

/** Small, fixed table — matches every other connector-internal sizing choice (no malloc). */
constexpr size_t MAX_REGISTERED_COMMANDS = 8;

/**
 * Register a client-defined command name (matched against the MQTT command
 * topic's suffix, e.g. "toggle_relay" for {tenant}/command/device/{uid}/toggle_relay).
 * Built-in commands ("ping", "specs_refresh", "restart") cannot be overridden
 * this way and always take priority if a client tries to reuse one of those names.
 *
 * Returns:
 *  - ESP_OK on success (including overwrite of an existing custom registration)
 *  - ESP_ERR_INVALID_ARG if name/handler is null/empty
 *  - ESP_ERR_NOT_ALLOWED if name collides with a built-in
 *  - ESP_ERR_NO_MEM if the fixed registration table is full
 */
esp_err_t registerCommand(const char* name, CommandHandler handler, void* user_ctx = nullptr);

bool publishCommandAck(const char* suffix, const char* command_id, const char* message);

/**
 * Publish a command response on {tenant}/response/device/{uid}/{suffix} — the
 * same mechanism handleMqttMessage() uses for the built-in commands. Custom
 * command handlers (registered via registerCommand()) call this to report
 * their outcome.
 *
 * On failure (ok=false), `code` must be one of the backend rejection enum values:
 *   unsupported | bad_args | not_permitted | busy | interlock | internal | other
 * Use `other` for device-specific failures and put detail in `message`.
 */
bool publishCommandResponse(const char* suffix, const char* command_id, bool ok, const char* code,
                             const char* message);

/**
 * Like publishCommandResponse(), but embeds a JSON object as result (or null when result is null).
 * Takes ownership of result — it is freed after publishing.
 */
bool publishCommandResponseWithResult(const char* suffix, const char* command_id, bool ok,
                                      const char* code, const char* message, cJSON* result);

}  // namespace heapwatch::device_command
