#pragma once
#include <stdint.h>

namespace ScheduleData {

// ============================================================
//  Schedule data - Replay 2026 (source: assets/replay2026_schedule.json)
//
//  Each parallel session in a "talks" block becomes its own
//  SchedEvent row.  Events sharing a start_min are grouped by
//  slot_start() so the time is printed only on the first row.
//
//  In production, PSRAM-backed data is parsed from live API MessagePack.
// ============================================================

#define SCHED_MAX_TALKS     4
#define SCHED_MAX_EVENTS   40
#define SCHED_MAX_DAYS      3
#define SCHED_TITLE_MAX   104
#define SCHED_ROOM_MAX     48
#define SCHED_SPEAKER_MAX 104
#define SCHED_LABEL_MAX    12
#define SCHED_DESC_MAX    192

typedef enum : uint8_t {
    SCHEDEVT_TALK,
    SCHEDEVT_WORKSHOP,
    SCHEDEVT_OTHER
} SchedEventType;

struct SchedTalk {
    const char* title;    // location / room shown in col 3
    const char* speaker;  // reserved; nullptr for now
};

struct SchedEvent {
    uint16_t       start_min;
    uint16_t       end_min;
    const char*    title;
    SchedEventType type;
    SchedTalk      talks[SCHED_MAX_TALKS];
    uint8_t        talk_count;
    const unsigned char* icon8;
    const char*    desc;     // optional event description (modal body)
};

struct SchedDay {
    const char*      label;
    SchedEvent       events[SCHED_MAX_EVENTS];
    uint8_t          count;
};

inline void sched_fmt_time(uint16_t min, char* buf) {
    uint8_t h = min / 60, m = min % 60;
    buf[0] = '0' + h / 10; buf[1] = '0' + h % 10;
    buf[2] = ':';
    buf[3] = '0' + m / 10; buf[4] = '0' + m % 10;
    buf[5] = '\0';
}

const SchedDay* sched_days();
const char* sched_mode_label();
bool sched_is_personal_mode();
void sched_use_full_mode();
const char* sched_toggle_label();
const char* sched_empty_message();
bool sched_is_loading();
bool sched_needs_pairing();
uint16_t sched_total_events();
bool sched_show_custom_schedule_prompt();
bool sched_start_refresh(bool force = false);
bool sched_toggle_mode();

// Synchronously load the on-disk cache for the current mode (if not
// already active). Safe to call from the main loop. Lets the screen
// show data immediately without waiting on the background fetch task.
bool sched_ensure_cache_loaded();

// Short token for the status header — "", "SYNC", "OLD", "OFFLINE",
// "..." — based on current mode + runtime state. Empty string means
// "fresh data from server, nothing to flag."
const char* sched_status_short();

// Hidden filter — when set, ScheduleScreen renders only events whose
// room (talks[0].title) matches this string. Used by the map's
// section-confirm action to drill into "events in <room>" via the
// schedule app instead of the standalone MapFloorScreen. Pass nullptr
// or "" to clear. Returns the active filter (or "" when none).
void sched_set_room_filter(const char* room);
const char* sched_room_filter();
bool sched_filter_active();

static const SchedDay SCHED_DAYS[SCHED_MAX_DAYS] = {

  // -- May 5 - Workshop & Hackathon Day --------------------------------------
  { "May 5", {
    { 7*60+30, 9*60,    "Badge Pickup + Breakfast",
      SCHEDEVT_OTHER,   {{"Lobby/2nd Fl.", nullptr}}, 1, nullptr },

    { 9*60, 12*60+30,   "Beginner to Builder Bootcamp",
      SCHEDEVT_WORKSHOP,{{"Go", nullptr}}, 1, nullptr,
      "Accelerated intro to Temporal. Covers durable execution and crash-proof execution. No prior Temporal knowledge required." },

    { 9*60, 12*60+30,   "Beginner to Builder Bootcamp",
      SCHEDEVT_WORKSHOP,{{"Java [sold out]", nullptr}}, 1, nullptr,
      "Accelerated intro to Temporal. Covers durable execution and crash-proof execution. No prior Temporal knowledge required." },

    { 9*60, 17*60,      "Building Durable AI Agents + Versioning Workflows",
      SCHEDEVT_WORKSHOP,{{"Python AI/Versioning", nullptr}}, 1, nullptr,
      "Morning: build production-ready AI agents with OpenAI and Pydantic SDKs. Afternoon: versioning strategies for safely evolving Workflows and Workers." },

    { 9*60, 17*60,      "Building Durable AI Applications With Temporal",
      SCHEDEVT_WORKSHOP,{{"Python Nexus/AI", nullptr}}, 1, nullptr,
      "Connect Durable Executions across team, namespace, and cloud boundaries with Nexus. Afternoon covers production-ready AI agents." },

    { 9*60, 17*60,      "Hackathon",
      SCHEDEVT_OTHER,   {}, 0, nullptr,
      "Experienced Temporal users form teams to build projects for the code exchange." },

    {12*60+30, 13*60+30,"Lunch",
      SCHEDEVT_OTHER,   {{"2nd Floor", nullptr}}, 1, nullptr },

    {13*60+30, 17*60,   "Beginner to Builder Bootcamp (Afternoon Session)",
      SCHEDEVT_WORKSHOP,{{"Go", nullptr}}, 1, nullptr,
      "Continuation of the morning Go workshop session." },

    {13*60+30, 17*60,   "Beginner to Builder Bootcamp (Afternoon Session)",
      SCHEDEVT_WORKSHOP,{{"Java [sold out]", nullptr}}, 1, nullptr,
      "Continuation of the morning Java workshop session." },

    {17*60, 19*60,      "Reception",
      SCHEDEVT_OTHER,   {{"Lobby", nullptr}}, 1, nullptr },
  }, 10 },

  // -- May 6 - Conference Day 1 -----------------------------------------------
  { "May 6", {
    { 7*60,  8*60,      "Replay 5K Fun Run",
      SCHEDEVT_OTHER,   {{"Cupid's Span", nullptr}}, 1, nullptr },

    { 7*60,  9*60,      "Badge Pickup + Breakfast",
      SCHEDEVT_OTHER,   {{"Lobby/Downstairs", nullptr}}, 1, nullptr },

    { 9*60, 10*60,      "Keynote",
      SCHEDEVT_TALK,    {{"The Hangar", nullptr}}, 1, nullptr },

    {10*60, 17*60,      "Birds of a Feather",
      SCHEDEVT_OTHER,   {{"The Expanse", nullptr}}, 1, nullptr },

    {10*60, 10*60+30,   "Break / Go Upstairs",
      SCHEDEVT_OTHER,   {}, 0, nullptr },

    // 10:30-11:15 parallel sessions
    {10*60+30, 11*60+15,"Orchestrating Data Pipelines at Scale with Temporal at Fivetran",
      SCHEDEVT_TALK,    {{"Pulsar", nullptr}}, 1, nullptr },

    {10*60+30, 11*60+15,"When Money Moves",
      SCHEDEVT_TALK,    {{"Quasar", nullptr}}, 1, nullptr },

    {10*60+30, 11*60+15,"Agentic Data Infra: How Instacart is Leveraging Temporal",
      SCHEDEVT_TALK,    {{"Nova", nullptr}}, 1, nullptr },

    {10*60+30, 11*60+15,"How we built a real-time voice agent with Temporal",
      SCHEDEVT_TALK,    {{"Nebula", nullptr}}, 1, nullptr },

    {10*60+30, 11*60+15,"Announcing New AI Capabilities",
      SCHEDEVT_TALK,    {{"Persenne", nullptr}}, 1, nullptr },

    {10*60+30, 12*60,   "Workshop: Secure Your Temporal Agents with Tailscale",
      SCHEDEVT_WORKSHOP,{{"Hyperion", nullptr}}, 1, nullptr },

    {11*60+15, 11*60+45,"Break",
      SCHEDEVT_OTHER,   {}, 0, nullptr },

    // 11:45-12:30 parallel sessions
    {11*60+45, 12*60+30,"Temporal and Autonomous Vehicles Infrastructure at Nvidia",
      SCHEDEVT_TALK,    {{"Quasar", nullptr}}, 1, nullptr },

    {11*60+45, 12*60+30,"From Normalizing Complexity to Recognizing the Price",
      SCHEDEVT_TALK,    {{"Nebula", nullptr}}, 1, nullptr },

    {11*60+45, 12*60+30,"The Future Is Deterministic - Building the Next Generation on Temporal",
      SCHEDEVT_TALK,    {{"Nova", nullptr}}, 1, nullptr },

    {11*60+45, 12*60+30,"Free Your Mind and Your App Will Follow",
      SCHEDEVT_TALK,    {{"Persenne", nullptr}}, 1, nullptr },

    {12*60+30, 13*60+30,"Lunch",
      SCHEDEVT_OTHER,   {{"Downstairs", nullptr}}, 1, nullptr },

    {13*60+30, 13*60+45,"Break / Go Upstairs",
      SCHEDEVT_OTHER,   {}, 0, nullptr },

    // 13:45-14:30 parallel sessions
    {13*60+45, 14*60+30,"Rainbow Deployments in The Cloud",
      SCHEDEVT_TALK,    {{"Nebula", nullptr}}, 1, nullptr },

    {13*60+45, 14*60+30,"Confidently Shipping with Temporal",
      SCHEDEVT_TALK,    {{"Nova", nullptr}}, 1, nullptr },

    {13*60+45, 14*60+30,"Durable Job Processing with Standalone Activities",
      SCHEDEVT_TALK,    {{"Persenne", nullptr}}, 1, nullptr },

    {13*60+45, 14*60+30,"How OpenAI uses Temporal",
      SCHEDEVT_TALK,    {{"Quasar", nullptr}}, 1, nullptr },

    {13*60+45, 14*60+30,"Engineering AI Applications: A Conversation with AI Leaders",
      SCHEDEVT_TALK,    {{"Pulsar", nullptr}}, 1, nullptr },

    {14*60+30, 15*60,   "Break",
      SCHEDEVT_OTHER,   {}, 0, nullptr },

    // 15:00-15:45 parallel sessions
    {15*60, 15*60+45,   "The Path to Temporal General Availability at Netflix",
      SCHEDEVT_TALK,    {{"Quasar", nullptr}}, 1, nullptr },

    {15*60, 15*60+45,   "Reliable Migration of 4+ million CPU Cores to Kubernetes",
      SCHEDEVT_TALK,    {{"Nova", nullptr}}, 1, nullptr },

    {15*60, 15*60+45,   "Lessons Learned Using Temporal to Run Temporal Cloud",
      SCHEDEVT_TALK,    {{"Persenne", nullptr}}, 1, nullptr },

    {15*60, 15*60+45,   "Lightning Round Talks",
      SCHEDEVT_TALK,    {{"Pulsar", nullptr}}, 1, nullptr },

    {15*60, 15*60+45,   "Taming AI Agents with Output.ai: An Open-Source Framework Built on Temporal",
      SCHEDEVT_TALK,    {{"Nebula", nullptr}}, 1, nullptr },

    {15*60+45, 16*60+15,"Break",
      SCHEDEVT_OTHER,   {}, 0, nullptr },

    // 16:15-17:00 parallel sessions
    {16*60+15, 17*60,   "Powering AI Agents with Temporal & kgoose: Lessons from Block",
      SCHEDEVT_TALK,    {{"Quasar", nullptr}}, 1, nullptr },

    {16*60+15, 17*60,   "Building Agentic Consumer Products with Temporal",
      SCHEDEVT_TALK,    {{"Pulsar", nullptr}}, 1, nullptr },

    {16*60+15, 17*60,   "Resource Fairness: Implementing (and deprecating) a Lease Broker",
      SCHEDEVT_TALK,    {{"Nebula", nullptr}}, 1, nullptr },

    {16*60+15, 17*60,   "Build with Confidence: Announcing Major Worker Improvements",
      SCHEDEVT_TALK,    {{"Persenne", nullptr}}, 1, nullptr },

    {17*60, 19*60,      "Networking",
      SCHEDEVT_OTHER,   {{"Lobby", nullptr}}, 1, nullptr },
  }, 35 },

  // -- May 7 - Conference Day 2 -----------------------------------------------
  { "May 7", {
    { 7*60,  9*60,      "Badge Pickup + Breakfast",
      SCHEDEVT_OTHER,   {{"Lobby/Downstairs", nullptr}}, 1, nullptr },

    { 9*60, 10*60,      "Product Keynote",
      SCHEDEVT_TALK,    {{"The Hangar", nullptr}}, 1, nullptr },

    {10*60, 17*60,      "Birds of a Feather",
      SCHEDEVT_OTHER,   {{"The Expanse", nullptr}}, 1, nullptr },

    {10*60, 10*60+30,   "Break / Go Upstairs",
      SCHEDEVT_OTHER,   {}, 0, nullptr },

    // 10:30-11:15 parallel sessions
    {10*60+30, 11*60+15,"100 Temporal Mistakes (and how to avoid them)",
      SCHEDEVT_TALK,    {{"Pulsar", nullptr}}, 1, nullptr },

    {10*60+30, 11*60+15,"From Bottlenecks to Self-Service: Duolingo Workflow-as-a-Service with Nexus",
      SCHEDEVT_TALK,    {{"Quasar", nullptr}}, 1, nullptr },

    {10*60+30, 11*60+15,"How OpenAI Uses Codex to Change How We Build",
      SCHEDEVT_TALK,    {{"Nebula", nullptr}}, 1, nullptr },

    {10*60+30, 11*60+15,"A New Foundation: Building with CHASM",
      SCHEDEVT_TALK,    {{"Persenne", nullptr}}, 1, nullptr },

    {10*60+30, 11*60+15,"Augmented Data Processing at Massive Scale with Temporal",
      SCHEDEVT_TALK,    {{"Nova", nullptr}}, 1, nullptr },

    {11*60+15, 11*60+45,"Break",
      SCHEDEVT_OTHER,   {}, 0, nullptr },

    // 11:45-12:30 parallel sessions
    {11*60+45, 12*60+30,"Durable Agents: Long-running AI workflows in a flaky world",
      SCHEDEVT_TALK,    {{"Quasar", nullptr}}, 1, nullptr },

    {11*60+45, 12*60+30,"Retool Agents: Building a Robust, Production-Ready AI Agent Platform",
      SCHEDEVT_TALK,    {{"Nova", nullptr}}, 1, nullptr },

    {11*60+45, 12*60+30,"Smarter Workflows, Leaner Costs: Inside Yum! Brands' Temporal Evolution",
      SCHEDEVT_TALK,    {{"Nebula", nullptr}}, 1, nullptr },

    {11*60+45, 12*60+30,"Cruising with Temporal: Navigating Road Bumps from Design to Production",
      SCHEDEVT_TALK,    {{"Persenne", nullptr}}, 1, nullptr },

    {12*60+30, 13*60+30,"Lunch",
      SCHEDEVT_OTHER,   {{"Downstairs", nullptr}}, 1, nullptr },

    {13*60+30, 13*60+45,"Break / Go Upstairs",
      SCHEDEVT_OTHER,   {}, 0, nullptr },

    // 13:45-14:30 parallel sessions
    {13*60+45, 14*60+30,"From Scale to Stability: High Availability in Temporal",
      SCHEDEVT_TALK,    {{"Persenne", nullptr}}, 1, nullptr },

    {13*60+45, 14*60+30,"How We Reliably Send A Million Messages Per Request",
      SCHEDEVT_TALK,    {{"Nebula", nullptr}}, 1, nullptr },

    {13*60+45, 14*60+30,"Agentic Ambient Listening at Abridge",
      SCHEDEVT_TALK,    {{"Nova", nullptr}}, 1, nullptr },

    {13*60+45, 14*60+30,"Scaling Temporal across the Enterprise: A Conversation with Platform Teams",
      SCHEDEVT_TALK,    {{"Pulsar", nullptr}}, 1, nullptr },

    {14*60+30, 15*60,   "Break",
      SCHEDEVT_OTHER,   {}, 0, nullptr },

    // 15:00-15:45 parallel sessions
    {15*60, 15*60+45,   "Temporal at Scale: Lessons in Migration and Security",
      SCHEDEVT_TALK,    {{"Nova", nullptr}}, 1, nullptr },

    {15*60, 15*60+45,   "Beyond the State Machine: scaling order orchestration at Booking.com",
      SCHEDEVT_TALK,    {{"Quasar", nullptr}}, 1, nullptr },

    {15*60, 15*60+45,   "Announcing Priority & Fairness for Temporal",
      SCHEDEVT_TALK,    {{"Persenne", nullptr}}, 1, nullptr },

    {15*60, 15*60+45,   "Being Creative with Temporal: Tales from the Trenches",
      SCHEDEVT_TALK,    {{"Nebula", nullptr}}, 1, nullptr },

    {15*60, 15*60+45,   "Lightning Round Talks",
      SCHEDEVT_TALK,    {{"Pulsar", nullptr}}, 1, nullptr },

    {15*60+45, 16*60+15,"Break",
      SCHEDEVT_OTHER,   {}, 0, nullptr },

    // 16:15-17:00 parallel sessions
    {16*60+15, 17*60,   "Streaming Messages from Temporal Workers",
      SCHEDEVT_TALK,    {{"Nebula", nullptr}}, 1, nullptr },

    {16*60+15, 17*60,   "Supercharging Cloud Orchestration using Temporal Open Source",
      SCHEDEVT_TALK,    {{"Quasar", nullptr}}, 1, nullptr },

    {16*60+15, 17*60,   "From Nothing to Everything: Managing Datacenters with AI",
      SCHEDEVT_TALK,    {{"Nova", nullptr}}, 1, nullptr },

    {16*60+15, 17*60,   "Bridging the CHASM: A Deep Dive into Temporal Server's State Machine SDK",
      SCHEDEVT_TALK,    {{"Persenne", nullptr}}, 1, nullptr },

    {16*60+15, 17*60,   "How Temporal Powers Salesforce's AI-Driven DDoS Protection",
      SCHEDEVT_TALK,    {{"Pulsar", nullptr}}, 1, nullptr },

    {17*60, 19*60,      "Closing & After Party",
      SCHEDEVT_OTHER,   {{"Lobby", nullptr}}, 1, nullptr },
  }, 33 },
};

}  // namespace ScheduleData
