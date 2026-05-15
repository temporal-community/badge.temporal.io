#include "Scheduler.h"
#include <cstring>

namespace {
constexpr uint32_t kSlowServiceWarnMs = 40;
}

Scheduler::Scheduler()
    : services_{},
      serviceCount_(0),
      loopCounter_(0),
      highDivisor_(1),
      normalDivisor_(1),
      lowDivisor_(1),
      resumeIndex_(0) {}

bool Scheduler::registerService(IService* service, ServicePriority priority) {
  if (service == nullptr || serviceCount_ >= kMaxServices) {
    return false;
  }

  for (uint8_t i = 0; i < serviceCount_; ++i) {
    if (services_[i].service == service) {
      return false;
    }
  }

  services_[serviceCount_].service = service;
  services_[serviceCount_].priority = priority;
  services_[serviceCount_].active = true;
  serviceCount_++;
  sortByPriority();
  return true;
}

void Scheduler::setExecutionDivisors(uint8_t high, uint8_t normal, uint8_t low) {
  highDivisor_ = high > 0 ? high : 1;
  normalDivisor_ = normal > 0 ? normal : 1;
  lowDivisor_ = low > 0 ? low : 1;
}

void Scheduler::runOnce() {
  loopCounter_++;
  for (uint8_t i = 0; i < serviceCount_; ++i) {
    IService* service = services_[i].service;
    if (service == nullptr || !services_[i].active) {
      continue;
    }

    if (!shouldRun(services_[i].priority)) {
      continue;
    }

    const uint32_t startMs = millis();
    service->service();
    const uint32_t elapsedMs = millis() - startMs;
    if (elapsedMs >= kSlowServiceWarnMs) {
      Serial.printf("[Scheduler] slow service=%s priority=%u elapsed=%lu ms loop=%lu\n",
                    service->name() ? service->name() : "?",
                    (unsigned)services_[i].priority,
                    (unsigned long)elapsedMs,
                    (unsigned long)loopCounter_);
    }
  }
}

void Scheduler::runFor(uint32_t budgetMs) {
  if (serviceCount_ == 0) {
    delay(budgetMs);
    return;
  }

  const uint32_t start = millis();

  do {
    if (resumeIndex_ >= serviceCount_) {
      resumeIndex_ = 0;
    }

    ServiceEntry& entry = services_[resumeIndex_];
    resumeIndex_++;

    if (entry.service != nullptr && entry.active) {
      entry.service->service();
    }
  } while ((millis() - start) < budgetMs);
}

bool Scheduler::shouldRun(ServicePriority priority) const {
  switch (priority) {
    case ServicePriority::kHigh:
      return (loopCounter_ % highDivisor_) == 0;
    case ServicePriority::kNormal:
      return (loopCounter_ % normalDivisor_) == 0;
    case ServicePriority::kLow:
      return (loopCounter_ % lowDivisor_) == 0;
    default:
      return true;
  }
}

bool Scheduler::setServiceState(const char* name, bool active) {
  if (name == nullptr) return false;
  for (uint8_t i = 0; i < serviceCount_; ++i) {
    if (services_[i].service && strcmp(services_[i].service->name(), name) == 0) {
      services_[i].active = active;
      return true;
    }
  }
  return false;
}

bool Scheduler::setServiceState(IService* service, bool active) {
  if (service == nullptr) return false;
  for (uint8_t i = 0; i < serviceCount_; ++i) {
    if (services_[i].service == service) {
      services_[i].active = active;
      return true;
    }
  }
  return false;
}

void Scheduler::printTasks() const {
  Serial.printf("=== Scheduler Tasks (%d/%d) ===\n", serviceCount_, kMaxServices);
  for (uint8_t i = 0; i < serviceCount_; ++i) {
    if (services_[i].service) {
      const char* state = services_[i].active ? "RUNNING" : "PAUSED ";
      const char* priorityStr = "";
      switch (services_[i].priority) {
        case ServicePriority::kHigh: priorityStr = "HIGH  "; break;
        case ServicePriority::kNormal: priorityStr = "NORMAL"; break;
        case ServicePriority::kLow: priorityStr = "LOW   "; break;
      }
      Serial.printf("[%d] %-15s | Priority: %s | State: %s\n", 
                    i, services_[i].service->name(), priorityStr, state);
    }
  }
  Serial.println("=================================");
}

void Scheduler::sortByPriority() {
  if (serviceCount_ <= 1) {
    return;
  }

  for (uint8_t i = 0; i < serviceCount_ - 1; ++i) {
    for (uint8_t j = 0; j < serviceCount_ - i - 1; ++j) {
      ServiceEntry& left = services_[j];
      ServiceEntry& right = services_[j + 1];
      if (left.service == nullptr || right.service == nullptr) {
        continue;
      }

      if (static_cast<uint8_t>(left.priority) >
          static_cast<uint8_t>(right.priority)) {
        ServiceEntry tmp = left;
        left = right;
        right = tmp;
      }
    }
  }
}
