// OTAService.h — low-priority Scheduler service that drives BadgeOTA
// and AssetRegistry daily polls.
//
// Both modules guard themselves against the 24h cooldown internally,
// so this just forwards `service()` ticks at a slow rate (~1 Hz is
// fine — the work happens at most once a day). Also handles the
// fresh-install rollback marker once the GUI has been alive for 30 s.

#pragma once

#include "../infra/Scheduler.h"

namespace ota {

class OTAService : public IService {
 public:
  void service() override;
  const char* name() const override { return "OTA"; }
};

extern OTAService otaService;

}  // namespace ota
