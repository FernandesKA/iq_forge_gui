#include "device_manager.h"

#include "hackrf_device.h"
#include "iq_forge_device.h"
#include "pluto_device.h"

namespace iqforge {

std::unique_ptr<IDevice> createDevice(DeviceKind kind) {
  switch (kind) {
    case DeviceKind::PlutoSDR: return std::make_unique<PlutoDevice>();
    case DeviceKind::HackRF: return std::make_unique<HackRFDevice>();
    case DeviceKind::IqForge: return std::make_unique<IqForgeDevice>();
  }
  return nullptr;
}

bool DeviceManager::connect(const DeviceConfig& cfg, std::string& errorOut) {
  disconnect();
  auto dev = createDevice(cfg.kind);
  if (!dev) {
    errorOut = "Unknown device kind";
    return false;
  }
  if (!dev->open(cfg, errorOut)) {
    return false;
  }
  device_ = std::move(dev);
  return true;
}

void DeviceManager::disconnect() {
  if (device_) {
    device_->close();
    device_.reset();
  }
}

bool DeviceManager::pollHealth() {
  if (!device_ || !device_->isOpen()) return false;

  constexpr auto kHealthCheckInterval = std::chrono::seconds(1);
  auto now = std::chrono::steady_clock::now();
  if (now - lastHealthCheck_ < kHealthCheckInterval) return false;
  lastHealthCheck_ = now;

  if (device_->checkAlive()) return false;

  disconnect();
  return true;
}

} // namespace iqforge
