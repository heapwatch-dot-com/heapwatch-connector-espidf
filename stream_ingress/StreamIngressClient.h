#pragma once

#include <cstddef>
#include <cstdint>

#include "../device_credentials/DeviceCredentialsStore.h"

namespace heapwatch::stream_ingress {

using FsUploadChunkHandler = void (*)(const uint8_t* data, size_t len);

/** Set stream API key from NVS provisioning (set during enrollment confirm). */
void setStreamApiKey(const char* stream_api_key);

/** Initialize stream ingress (mutex/queue only; worker starts on first TCP need). */
void init();

/** Request opening a mux TCP stream to backend-stream-ingress (async). Enables log capture. */
void requestOpen();

/** Request closing the active stream connection and log capture. */
void requestClose();

/**
 * Lazily request a shared ingress TCP session (Console + file browser).
 * Does not enable log capture. Returns false when stream ingress is not configured.
 */
bool ensureTcpSession();

/** True while a stream TCP session is connected. */
bool isActive();

/** True while stream capture is armed (between stream_open and stream_close). */
bool isCaptureActive();

/**
 * Queue a MQTT-shaped log JSON line for the stream "logs" channel (NDJSON coalesce).
 */
void enqueueLogJson(const char* json_line);

/** Queue a mux DATA frame on channel_id (max 4 KiB payload). */
bool sendMuxDataFrame(const char* channel_id, const uint8_t* payload, size_t payload_len);

/** Queue a mux CTRL frame (end-of-stream marker) on channel_id. */
bool sendMuxCtrlFrame(const char* channel_id);

/** Send a JSON filesystem result on the mux "file_browser" channel. */
bool sendFileBrowserJson(const char* json_line);

/** Send a BLE scan peer/complete JSON line on the mux "ble_scan" channel. */
bool sendBleScanJson(const char* json_line);

/** Send a BLE lifecycle trace JSON line on the mux "ble_events" channel. */
bool sendBleEventJson(const char* json_line);

/** Stream a binary download chunk on the mux "fs_download/{command_id}" channel. */
bool sendFsDownloadChunk(const char* command_id, const uint8_t* data, size_t len);

/**
 * Drain queued mux frames to the TCP socket.
 * Safe to call from hw_stream_ingress (including while running FS work).
 */
void flushOutbound();

/** Register handler for inbound mux upload chunks on channel "fs_upload/{command_id}". */
void setFsUploadChunkHandler(FsUploadChunkHandler handler);

/** Queue a one-shot command manifest upload on hw_streaming (non-blocking). */
void requestManifestUpload();

}  // namespace heapwatch::stream_ingress
