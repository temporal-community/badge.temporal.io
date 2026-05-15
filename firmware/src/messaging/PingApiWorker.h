// #pragma once

// #include <stddef.h>
// #include <stdint.h>

// #include "freertos/FreeRTOS.h"
// #include "freertos/task.h"

// // #include "../api/BadgeAPI.h"

// namespace PingApiWorker {

// static constexpr BaseType_t kWorkerCore = 0;
// static constexpr UBaseType_t kWorkerPriority = 1;
// static constexpr uint32_t kTlsWorkerStackBytes = 16384;

// void* psramPreferredAlloc(size_t bytes);

// bool resolveTargetTicket(const char* targetUid,
//                          char* targetTicket,
//                          size_t targetTicketCap,
//                          const char* logTag);

// SendPingResult sendJson(const char* sourceUid,
//                         const char* targetUid,
//                         char* targetTicket,
//                         size_t targetTicketCap,
//                         const char* activityType,
//                         const char* dataJson,
//                         const char* logTag);

// void logTaskDone(const char* logTag, uint32_t startedMs);

// }  // namespace PingApiWorker
