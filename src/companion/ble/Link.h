#pragma once
#if CROSSPOINT_COMPANION

// One BLE connection as seen by Session: an outgoing frame queue and a
// disconnect request. Implemented by ble/BleServer on the device and by a
// recording fake in host tests.

#include <cstddef>
#include <cstdint>

namespace companion {

class LinkPort {
 public:
  virtual ~LinkPort() = default;
  // Queues one complete frame (header + payload) for notification. Returns false
  // when the queue is full; the caller retries on a later tick.
  virtual bool send(const uint8_t* frame, size_t len) = 0;
  virtual bool canSend() const = 0;
  // Drops the link once every queued frame has been notified.
  virtual void requestDisconnect() = 0;
  virtual uint16_t mtu() const = 0;
};

}  // namespace companion

#endif  // CROSSPOINT_COMPANION
