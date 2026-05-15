#ifndef SCHEDULER_H
#define SCHEDULER_H

#include <Arduino.h>
#include <stdint.h>

enum class ServicePriority : uint8_t {
  kHigh = 0,
  kNormal = 1,
  kLow = 2,
};

class IService {
 public:
  virtual ~IService() = default;
  virtual const char* name() const = 0;
  virtual void service() = 0;
};

class Scheduler {
 public:
  static constexpr uint8_t kMaxServices = 16;

  Scheduler();
  bool registerService(IService* service, ServicePriority priority);
  void setExecutionDivisors(uint8_t high, uint8_t normal, uint8_t low);
  void runOnce();
  void runFor(uint32_t budgetMs);
  
  bool setServiceState(const char* name, bool active);
  bool setServiceState(IService* service, bool active);
  void printTasks() const;

 private:
  bool shouldRun(ServicePriority priority) const;
  void sortByPriority();

  struct ServiceEntry {
    IService* service;
    ServicePriority priority;
    bool active;
  };

  ServiceEntry services_[kMaxServices];
  uint8_t serviceCount_;
  uint32_t loopCounter_;
  uint8_t highDivisor_;
  uint8_t normalDivisor_;
  uint8_t lowDivisor_;
  uint8_t resumeIndex_;
};

#endif
