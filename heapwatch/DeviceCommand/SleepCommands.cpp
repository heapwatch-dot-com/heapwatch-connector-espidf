#include "heapwatch/DeviceCommand/SleepCommands.h"

#include <cstdlib>

#include "cJSON.h"

#include "heapwatch/DeviceCommand/DeviceCommandHandler.h"
#include "heapwatch/DeviceCommand/GoingDark/GoingDarkPipeline.h"

namespace Heapwatch::DeviceCommand {

namespace {

bool parseDurationSeconds(const char* payload, size_t payload_len, uint32_t* duration_s) {
    *duration_s = 0;
    if (payload_len == 0) {
        return true;
    }

    cJSON* root = cJSON_ParseWithLength(payload, static_cast<int>(payload_len));
    if (root == nullptr) {
        return false;
    }

    const cJSON* duration = cJSON_GetObjectItem(root, "duration_s");
    if (cJSON_IsNumber(duration)) {
        if (duration->valuedouble < 0 || duration->valuedouble > 86400) {
            cJSON_Delete(root);
            return false;
        }
        *duration_s = static_cast<uint32_t>(duration->valuedouble);
    }

    cJSON_Delete(root);
    return true;
}

}  // namespace

void handleSleepLight(const char* suffix, char* command_id, const char* payload,
                      size_t payload_len) {
    uint32_t duration_s = 0;
    if (!parseDurationSeconds(payload, payload_len, &duration_s)) {
        publishCommandResponse(suffix, command_id, false, "bad_args", "duration_s must be 0-86400");
        free(command_id);
        return;
    }

    if (!Heapwatch::DeviceCommand::GoingDark::scheduleSleep(suffix, command_id, Heapwatch::DeviceCommand::GoingDark::SleepKind::Light,
                                              duration_s)) {
        publishCommandResponse(suffix, command_id, false, "busy", "Going-dark pipeline busy");
        free(command_id);
        return;
    }

    free(command_id);
}

void handleSleepDeep(const char* suffix, char* command_id, const char* payload, size_t payload_len) {
    uint32_t duration_s = 0;
    if (!parseDurationSeconds(payload, payload_len, &duration_s)) {
        publishCommandResponse(suffix, command_id, false, "bad_args", "duration_s must be 0-86400");
        free(command_id);
        return;
    }

    if (!Heapwatch::DeviceCommand::GoingDark::scheduleSleep(suffix, command_id, Heapwatch::DeviceCommand::GoingDark::SleepKind::Deep,
                                              duration_s)) {
        publishCommandResponse(suffix, command_id, false, "busy", "Going-dark pipeline busy");
        free(command_id);
        return;
    }

    free(command_id);
}

}  // namespace Heapwatch::DeviceCommand
