#pragma once

#include "../infra/Scheduler.h"

class MicroPythonMatrixService : public IService {
 public:
  void service() override;
  const char* name() const override;

  void beginOverride();
  void endOverride();
  bool isOverridden() const;
  bool hasCallback() const;
};

class MicroPythonMatrixOverride {
 public:
  explicit MicroPythonMatrixOverride(MicroPythonMatrixService& svc) : svc_(svc) {
    svc_.beginOverride();
  }
  ~MicroPythonMatrixOverride() { svc_.endOverride(); }

  MicroPythonMatrixOverride(const MicroPythonMatrixOverride&) = delete;
  MicroPythonMatrixOverride& operator=(const MicroPythonMatrixOverride&) = delete;

 private:
  MicroPythonMatrixService& svc_;
};
