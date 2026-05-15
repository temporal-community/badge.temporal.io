// #include "PingApiWorker.h"

// #include <Arduino.h>
// #include <cstdlib>
// #include <cstring>

// namespace PingApiWorker {

// void* psramPreferredAlloc(size_t bytes) {
//     void* p = ps_malloc(bytes);
//     return p ? p : malloc(bytes);
// }

// bool resolveTargetTicket(const char* targetUid,
//                          char* targetTicket,
//                          size_t targetTicketCap,
//                          const char* logTag) {
//     if (!targetTicket || targetTicketCap == 0) return false;
//     if (targetTicket[0]) return true;
//     if (!targetUid || !targetUid[0]) return false;

//     FetchBadgeXBMResult info = BadgeAPI::fetchBadgeXBM(targetUid);
//     if (info.buf) {
//         free(info.buf);
//         info.buf = nullptr;
//     }
//     if (!info.ok || !info.ticketUuid[0]) {
//         Serial.printf("[%s] peer ticket lookup failed peer=%s http=%d ok=%d\n",
//                       logTag ? logTag : "PingApi",
//                       targetUid,
//                       info.httpCode,
//                       info.ok ? 1 : 0);
//         return false;
//     }

//     strncpy(targetTicket, info.ticketUuid, targetTicketCap - 1);
//     targetTicket[targetTicketCap - 1] = '\0';
//     Serial.printf("[%s] peer ticket lookup ok peer=%s\n",
//                   logTag ? logTag : "PingApi",
//                   targetUid);
//     return true;
// }

// SendPingResult sendJson(const char* sourceUid,
//                         const char* targetUid,
//                         char* targetTicket,
//                         size_t targetTicketCap,
//                         const char* activityType,
//                         const char* dataJson,
//                         const char* logTag) {
//     SendPingResult result = {};
//     if (!sourceUid || !sourceUid[0] ||
//         !activityType || !activityType[0] ||
//         !dataJson || !dataJson[0]) {
//         return result;
//     }

//     if (!resolveTargetTicket(targetUid, targetTicket, targetTicketCap, logTag)) {
//         return result;
//     }

//     return BadgeAPI::sendPing(sourceUid, targetTicket, activityType, dataJson);
// }

// void logTaskDone(const char* logTag, uint32_t startedMs) {
//     Serial.printf("[%s] api task elapsed=%lu ms stack_hwm_bytes=%u core=%d\n",
//                   logTag ? logTag : "PingApi",
//                   (unsigned long)(millis() - startedMs),
//                   (unsigned)uxTaskGetStackHighWaterMark(nullptr),
//                   xPortGetCoreID());
// }

// }  // namespace PingApiWorker
