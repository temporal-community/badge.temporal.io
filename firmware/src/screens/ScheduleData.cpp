#include "ScheduleData.h"

#include <Arduino.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "../api/DataCache.h"
#include "../api/MsgPackReader.h"
#include "../infra/Filesystem.h"
#include "../infra/PsramAllocator.h"

namespace ScheduleData {
namespace {

constexpr const char* kYourCachePath = "/schedule_your_v2.msgpack";
constexpr const char* kFullCachePath = "/schedule_full_v2.msgpack";
constexpr size_t kScheduleCacheMax = 72 * 1024;
constexpr uint32_t kRefreshIntervalMs = 60UL * 60UL * 1000UL;

enum class ScheduleStatus : uint8_t {
  kStatic,
  kLoading,
  kYour,
  kFull,
  kCachedYour,
  kCachedFull,
  kError,
};

enum class ScheduleMode : uint8_t {
  kPersonal,
  kFull,
};

static const SchedDay EMPTY_DAYS[SCHED_MAX_DAYS] = {
  {"May 5", {}, 0},
  {"May 6", {}, 0},
  {"May 7", {}, 0},
};

struct RuntimeStore {
  SchedDay days[SCHED_MAX_DAYS];
  char labels[SCHED_MAX_DAYS][SCHED_LABEL_MAX];
  char dateKeys[SCHED_MAX_DAYS][11];
  char titles[SCHED_MAX_DAYS][SCHED_MAX_EVENTS][SCHED_TITLE_MAX];
  char rooms[SCHED_MAX_DAYS][SCHED_MAX_EVENTS][SCHED_ROOM_MAX];
  char speakers[SCHED_MAX_DAYS][SCHED_MAX_EVENTS][SCHED_SPEAKER_MAX];
  char descs[SCHED_MAX_DAYS][SCHED_MAX_EVENTS][SCHED_DESC_MAX];
};

RuntimeStore* s_runtime[2] = {nullptr, nullptr};
volatile int8_t s_activeRuntime = -1;
volatile bool s_refreshInProgress = false;
volatile ScheduleStatus s_status = ScheduleStatus::kStatic;
volatile ScheduleMode s_mode = ScheduleMode::kFull;
uint32_t s_lastRefreshAttemptMs = 0;

bool ensureRuntimeStores() {
  for (uint8_t i = 0; i < 2; i++) {
    if (s_runtime[i]) continue;
    s_runtime[i] = static_cast<RuntimeStore*>(
        BadgeMemory::allocPreferPsram(sizeof(RuntimeStore)));
    if (!s_runtime[i]) {
      Serial.println("[Schedule] runtime store alloc failed");
      return false;
    }
    memset(s_runtime[i], 0, sizeof(RuntimeStore));
  }
  return true;
}

void copyText(char* dst, size_t cap, const char* src) {
  if (!dst || cap == 0) return;
  if (!src) src = "";
  strncpy(dst, src, cap - 1);
  dst[cap - 1] = '\0';
}

int parse2(const char* s) {
  if (!s || s[0] < '0' || s[0] > '9' || s[1] < '0' || s[1] > '9') {
    return -1;
  }
  return (s[0] - '0') * 10 + (s[1] - '0');
}

uint16_t parseTimeMinutes(const char* time, const char* iso) {
  const char* p = nullptr;
  if (time && std::strlen(time) >= 5) {
    p = time;
  } else if (iso && std::strlen(iso) >= 16) {
    p = iso + 11;
  }
  if (!p || p[2] != ':') return 0;
  const int h = parse2(p);
  const int m = parse2(p + 3);
  if (h < 0 || h > 23 || m < 0 || m > 59) return 0;
  return static_cast<uint16_t>(h * 60 + m);
}

void labelFromDate(const char* date, char* out, size_t cap) {
  static const char* kMonths[] = {
      "Jan", "Feb", "Mar", "Apr", "May", "Jun",
      "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  if (!out || cap == 0) return;
  out[0] = '\0';
  if (!date || std::strlen(date) < 10) return;
  const int month = parse2(date + 5);
  const int day = parse2(date + 8);
  if (month < 1 || month > 12 || day < 1 || day > 31) return;
  snprintf(out, cap, "%s %d", kMonths[month - 1], day);
}

void initStore(RuntimeStore& store) {
  memset(&store, 0, sizeof(store));
  static const char* kDefaultDates[SCHED_MAX_DAYS] = {
      "2026-05-05", "2026-05-06", "2026-05-07"};

  for (uint8_t d = 0; d < SCHED_MAX_DAYS; d++) {
    copyText(store.labels[d], sizeof(store.labels[d]), SCHED_DAYS[d].label);
    copyText(store.dateKeys[d], sizeof(store.dateKeys[d]), kDefaultDates[d]);
    store.days[d].label = store.labels[d];
    store.days[d].count = 0;
  }
}

int findDay(RuntimeStore& store, const char* date) {
  if (!date || !date[0]) return -1;
  for (uint8_t d = 0; d < SCHED_MAX_DAYS; d++) {
    if (strcmp(store.dateKeys[d], date) == 0) return d;
  }
  for (uint8_t d = 0; d < SCHED_MAX_DAYS; d++) {
    if (store.days[d].count == 0) {
      copyText(store.dateKeys[d], sizeof(store.dateKeys[d]), date);
      labelFromDate(date, store.labels[d], sizeof(store.labels[d]));
      if (!store.labels[d][0]) copyText(store.labels[d], sizeof(store.labels[d]), date);
      return d;
    }
  }
  return -1;
}

SchedEventType typeFromCode(int64_t type) {
  if (type == 0) return SCHEDEVT_TALK;
  if (type == 1) return SCHEDEVT_WORKSHOP;
  return SCHEDEVT_OTHER;
}

int compareEvents(const SchedEvent& a, const SchedEvent& b) {
  if (a.start_min != b.start_min) return (a.start_min < b.start_min) ? -1 : 1;
  if (a.end_min != b.end_min) return (a.end_min < b.end_min) ? -1 : 1;
  return strcmp(a.title ? a.title : "", b.title ? b.title : "");
}

void sortDay(SchedDay& day) {
  for (uint8_t i = 1; i < day.count; i++) {
    SchedEvent key = day.events[i];
    int j = i - 1;
    while (j >= 0 && compareEvents(key, day.events[j]) < 0) {
      day.events[j + 1] = day.events[j];
      j--;
    }
    day.events[j + 1] = key;
  }
}

uint16_t totalEvents(const RuntimeStore& store) {
  uint16_t total = 0;
  for (uint8_t d = 0; d < SCHED_MAX_DAYS; d++) {
    total += store.days[d].count;
  }
  return total;
}

bool parseScheduleMsgPack(const uint8_t* payload, size_t len,
                          RuntimeStore& store, bool requireEvents) {
  if (!payload || len == 0) return false;

  MsgPack::Reader rd(payload, len);
  uint32_t rootCount = 0;
  if (!rd.readArray(rootCount) || rootCount < 4) {
    Serial.println("[Schedule] compact MessagePack root invalid");
    return false;
  }

  uint64_t version = 0;
  if (!rd.readUInt(version) || version != 1) {
    Serial.println("[Schedule] compact MessagePack version invalid");
    return false;
  }
  if (!rd.skip() || !rd.skip()) {
    Serial.println("[Schedule] compact MessagePack metadata invalid");
    return false;
  }

  uint32_t eventCount = 0;
  if (!rd.readArray(eventCount)) {
    Serial.println("[Schedule] compact MessagePack events invalid");
    return false;
  }

  initStore(store);

  for (uint32_t i = 0; i < eventCount; i++) {
    uint32_t fieldCount = 0;
    if (!rd.readArray(fieldCount) || fieldCount < 7) {
      return false;
    }

    char date[11] = "";
    char startTime[6] = "";
    char endTime[6] = "";
    int64_t typeCode = 2;
    char title[SCHED_TITLE_MAX] = "";
    char room[SCHED_ROOM_MAX] = "";
    char speakerText[SCHED_SPEAKER_MAX] = "";
    char desc[SCHED_DESC_MAX] = "";

    if (!rd.readString(date, sizeof(date))) return false;
    if (!rd.readString(startTime, sizeof(startTime))) return false;
    if (!rd.readString(endTime, sizeof(endTime))) return false;
    if (!rd.readInt(typeCode)) return false;
    if (!rd.readString(title, sizeof(title))) return false;
    if (!rd.readString(room, sizeof(room))) return false;
    if (!rd.readString(speakerText, sizeof(speakerText))) return false;
    if (fieldCount >= 8) {
      if (!rd.readString(desc, sizeof(desc))) return false;
    }
    for (uint32_t extra = (fieldCount >= 8 ? 8 : 7); extra < fieldCount;
         extra++) {
      if (!rd.skip()) return false;
    }

    const int dayIndex = findDay(store, date);
    if (dayIndex < 0) continue;

    SchedDay& day = store.days[dayIndex];
    if (day.count >= SCHED_MAX_EVENTS) continue;

    const uint8_t idx = day.count;
    SchedEvent& dst = day.events[idx];

    copyText(store.titles[dayIndex][idx], sizeof(store.titles[dayIndex][idx]),
             title);
    copyText(store.rooms[dayIndex][idx], sizeof(store.rooms[dayIndex][idx]),
             room);
    copyText(store.speakers[dayIndex][idx], sizeof(store.speakers[dayIndex][idx]),
             speakerText);
    copyText(store.descs[dayIndex][idx], sizeof(store.descs[dayIndex][idx]),
             desc);

    dst.start_min = parseTimeMinutes(startTime, nullptr);
    dst.end_min = parseTimeMinutes(endTime, nullptr);
    dst.title = store.titles[dayIndex][idx];
    dst.type = typeFromCode(typeCode);
    dst.talk_count = store.rooms[dayIndex][idx][0] ? 1 : 0;
    dst.icon8 = nullptr;
    dst.desc  = store.descs[dayIndex][idx][0]
                    ? store.descs[dayIndex][idx]
                    : nullptr;
    for (uint8_t t = 0; t < SCHED_MAX_TALKS; t++) {
      dst.talks[t].title = nullptr;
      dst.talks[t].speaker = nullptr;
    }
    if (dst.talk_count > 0) {
      dst.talks[0].title = store.rooms[dayIndex][idx];
      dst.talks[0].speaker = store.speakers[dayIndex][idx][0]
          ? store.speakers[dayIndex][idx]
          : nullptr;
    }
    day.count++;
  }

  for (uint8_t d = 0; d < SCHED_MAX_DAYS; d++) {
    sortDay(store.days[d]);
  }

  for (uint32_t extra = 4; extra < rootCount; extra++) {
    if (!rd.skip()) return false;
  }

  return !requireEvents || totalEvents(store) > 0;
}

RuntimeStore* inactiveStore() {
  if (!ensureRuntimeStores()) return nullptr;
  const int8_t active = s_activeRuntime;
  return s_runtime[(active == 0) ? 1 : 0];
}

void applyStore(RuntimeStore* store, ScheduleStatus status) {
  if (!store) return;
  const int8_t next = (store == s_runtime[0]) ? 0 : 1;
  s_activeRuntime = next;
  s_status = status;
}

bool parseAndApply(const uint8_t* payload, size_t len, ScheduleStatus status,
                   bool requireEvents) {
  RuntimeStore* store = inactiveStore();
  if (!store || !parseScheduleMsgPack(payload, len, *store, requireEvents)) {
    return false;
  }
  applyStore(store, status);
  return true;
}

bool loadCache(const char* path, ScheduleStatus status, bool requireEvents) {
  char* buf = nullptr;
  size_t len = 0;
  if (!Filesystem::readFileAlloc(path, &buf, &len, kScheduleCacheMax)) {
    return false;
  }
  const bool ok = parseAndApply(reinterpret_cast<const uint8_t*>(buf), len,
                                status, requireEvents);
  free(buf);
  if (ok) {
    Serial.printf("[Schedule] loaded cache %s\n", path);
  }
  return ok;
}

void clearPersonalScheduleState() {
  s_activeRuntime = -1;
  s_lastRefreshAttemptMs = 0;
  Filesystem::removeFile(kYourCachePath);
}

}  // namespace

const SchedDay* sched_days() {
  const int8_t active = s_activeRuntime;
  if (active >= 0 && active < 2 && s_runtime[active]) {
    return s_runtime[active]->days;
  }
  if (s_mode == ScheduleMode::kPersonal) {
    return EMPTY_DAYS;
  }
  return SCHED_DAYS;
}

const char* sched_mode_label() {
  return s_mode == ScheduleMode::kPersonal ? "MINE" : "ALL";
}

bool sched_is_personal_mode() {
  return s_mode == ScheduleMode::kPersonal;
}

void sched_use_full_mode() {
  if (s_mode != ScheduleMode::kFull) {
    s_mode = ScheduleMode::kFull;
    s_activeRuntime = -1;
    s_lastRefreshAttemptMs = 0;
  }
  if (s_status == ScheduleStatus::kYour ||
      s_status == ScheduleStatus::kCachedYour) {
    s_status = ScheduleStatus::kStatic;
  }
}

const char* sched_toggle_label() {
  return "";
}

const char* sched_empty_message() {
  switch (s_status) {
    case ScheduleStatus::kLoading:
      return "Loading schedule";
    case ScheduleStatus::kYour:
    case ScheduleStatus::kCachedYour:
      return "No selected talks";
    case ScheduleStatus::kError:
      return "Schedule unavailable";
    default:
      return "No sessions";
  }
}

bool sched_is_loading() {
  return s_refreshInProgress || s_status == ScheduleStatus::kLoading;
}

bool sched_needs_pairing() {
  return false;
}

uint16_t sched_total_events() {
  const int8_t active = s_activeRuntime;
  if (active >= 0 && active < 2 && s_runtime[active]) {
    return totalEvents(*s_runtime[active]);
  }
  if (s_mode == ScheduleMode::kFull &&
      s_status != ScheduleStatus::kError) {
    uint16_t total = 0;
    for (uint8_t d = 0; d < SCHED_MAX_DAYS; d++) {
      total += SCHED_DAYS[d].count;
    }
    return total;
  }
  return 0;
}

bool sched_show_custom_schedule_prompt() {
  return false;
}

bool sched_start_refresh(bool force) {
  if (s_refreshInProgress) return false;
  const uint32_t now = millis();
  const bool hasRuntimeSchedule = s_activeRuntime >= 0;
  if (!force && hasRuntimeSchedule && s_lastRefreshAttemptMs != 0 &&
      (now - s_lastRefreshAttemptMs) < kRefreshIntervalMs) {
    return false;
  }
  s_lastRefreshAttemptMs = now;
  return sched_ensure_cache_loaded();
}

bool sched_toggle_mode() {
  sched_use_full_mode();
  return false;
}

bool sched_ensure_cache_loaded() {
  // Already showing live or cached data for this mode? Nothing to do.
  const int8_t active = s_activeRuntime;
  if (active >= 0 && active < 2 && s_runtime[active]) {
    return true;
  }
  const bool personal = (s_mode == ScheduleMode::kPersonal);
  const char* path = personal ? kYourCachePath : kFullCachePath;
  const ScheduleStatus st = personal ? ScheduleStatus::kCachedYour
                                     : ScheduleStatus::kCachedFull;
  if (loadCache(path, st, /*requireEvents=*/!personal)) {
    return true;
  }
  // FatFS cache miss — fall back to the embedded bundle so a fresh
  // badge immediately renders the static schedule shipped at flash time.
  if (!personal) {
    DataCache::ReadLock lock;
    DataCache::Span span = DataCache::schedule();
    if (span.data && span.len > 0 &&
        parseAndApply(span.data, span.len,
                      ScheduleStatus::kStatic, /*requireEvents=*/true)) {
      Serial.printf("[Schedule] loaded embedded fallback bundle (%u B)\n",
                    static_cast<unsigned>(span.len));
      return true;
    }
  }
  return false;
}

namespace {
char s_roomFilter[64] = {};
}  // namespace

void sched_set_room_filter(const char* room) {
  if (!room) {
    s_roomFilter[0] = '\0';
    return;
  }
  strncpy(s_roomFilter, room, sizeof(s_roomFilter) - 1);
  s_roomFilter[sizeof(s_roomFilter) - 1] = '\0';
}

const char* sched_room_filter() { return s_roomFilter; }

bool sched_filter_active() { return s_roomFilter[0] != '\0'; }

const char* sched_status_short() {
  // Refresh in progress wins — even on top of cached/static data —
  // so the user sees the "we're trying" hint as soon as a fetch
  // starts.
  if (s_refreshInProgress) return "SYNC";
  switch (s_status) {
    case ScheduleStatus::kLoading:    return "…";
    case ScheduleStatus::kCachedYour:
    case ScheduleStatus::kCachedFull: return "OLD";
    case ScheduleStatus::kError:      return "OFFLINE";
    case ScheduleStatus::kStatic:
    case ScheduleStatus::kYour:
    case ScheduleStatus::kFull:
    default:                          return "";
  }
}

}  // namespace ScheduleData
