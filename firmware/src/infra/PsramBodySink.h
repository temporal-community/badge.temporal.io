#pragma once

#include <Arduino.h>
#include <Stream.h>
#include <cstdlib>
#include <cstring>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "PsramAllocator.h"

namespace BadgeMemory {

// Append target for HTTPClient::writeToStream. Allocates the body buffer with
// allocPreferPsram so HTTPS responses do not use StreamString / getString(),
// which reserve the full Content-Length on internal heap alongside mbedTLS.
class PsramBodySink : public ::Stream {
 public:
  // `yieldEveryBytes` > 0: after each accumulated `yieldEveryBytes` of payload,
  // block briefly so the IDLE task on this CPU can satisfy the idle-task TWDT.
  // HTTPClient drains TLS in bursts; a 31 KB JSON manifest can otherwise sit in
  // read+memcpy for seconds without yielding.
  explicit PsramBodySink(size_t capBytes, size_t yieldEveryBytes = 0)
      : cap_(capBytes),
        yieldEvery_(yieldEveryBytes),
        nextYieldAt_(yieldEveryBytes > 0 ? yieldEveryBytes : 0) {
    if (capBytes == 0) return;
    buf_ = static_cast<char*>(allocPreferPsram(capBytes + 1));
    if (!buf_) return;
    buf_[0] = '\0';
  }

  ~PsramBodySink() override { std::free(buf_); }

  PsramBodySink(const PsramBodySink&) = delete;
  PsramBodySink& operator=(const PsramBodySink&) = delete;

  bool ok() const { return buf_ != nullptr; }
  size_t length() const { return len_; }
  const char* c_str() const { return buf_ ? buf_ : ""; }

  char* release() {
    char* p = buf_;
    buf_ = nullptr;
    len_ = 0;
    cap_ = 0;
    return p;
  }

  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }

  size_t write(uint8_t c) override { return write(&c, 1); }

  size_t write(const uint8_t* buffer, size_t size) override {
    if (!buf_ || size == 0) return 0;
    if (len_ >= cap_) return 0;
    const size_t room = cap_ - len_;
    const size_t n = (size < room) ? size : room;
    std::memcpy(buf_ + len_, buffer, n);
    len_ += n;
    buf_[len_] = '\0';
    while (yieldEvery_ > 0 && len_ >= nextYieldAt_) {
      nextYieldAt_ += yieldEvery_;
      vTaskDelay(pdMS_TO_TICKS(5));
    }
    if (n < size) return 0;
    return size;
  }

 private:
  size_t cap_{0};
  size_t yieldEvery_{0};
  size_t nextYieldAt_{0};
  size_t len_{0};
  char* buf_{nullptr};
};

}  // namespace BadgeMemory
