/*
 * adapters/vigem_adapter.cpp — IViGemPort implementation.
 */
#include "vigem_adapter.h"

// ── Raw ViGEm driver functions (defined in vigem.cpp / infra) ───────────────
extern HANDLE openVigemBus();
extern bool pluginTarget(HANDLE bus, unsigned long serial);
extern bool submitReportFast(HANDLE bus, unsigned long serial, const XUSB_REPORT& rpt,
                             HANDLE event);
extern void unplugTarget(HANDLE bus, unsigned long serial);
extern bool isVigemInstalled();

ViGEmAdapter::ViGEmAdapter() = default;

ViGEmAdapter::~ViGEmAdapter() { closeBus(); }

bool ViGEmAdapter::ensureBusOpen() {
    std::lock_guard<std::mutex> lk(busMtx_);
    if (busHandle_ != INVALID_HANDLE_VALUE) return true;
    busHandle_ = openVigemBus();
    return busHandle_ != INVALID_HANDLE_VALUE;
}

void ViGEmAdapter::closeBus() {
    std::lock_guard<std::mutex> lk(busMtx_);
    // Clean up all submit events
    for (auto& [serial, evt] : submitEvents_) {
        if (evt) CloseHandle(evt);
    }
    submitEvents_.clear();

    if (busHandle_ != INVALID_HANDLE_VALUE) {
        CloseHandle(busHandle_);
        busHandle_ = INVALID_HANDLE_VALUE;
    }
}

bool ViGEmAdapter::isBusOpen() const {
    std::lock_guard<std::mutex> lk(busMtx_);
    return busHandle_ != INVALID_HANDLE_VALUE;
}

bool ViGEmAdapter::pluginDevice(uint32_t serial) {
    std::lock_guard<std::mutex> lk(busMtx_);
    if (busHandle_ == INVALID_HANDLE_VALUE) return false;

    if (!pluginTarget(busHandle_, (unsigned long)serial)) return false;

    // Pre-allocate overlapped event for fast report submission
    HANDLE evt = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    submitEvents_[serial] = evt;
    return true;
}

void ViGEmAdapter::unplugDevice(uint32_t serial) {
    std::lock_guard<std::mutex> lk(busMtx_);
    if (busHandle_ == INVALID_HANDLE_VALUE) return;

    unplugTarget(busHandle_, (unsigned long)serial);

    auto it = submitEvents_.find(serial);
    if (it != submitEvents_.end()) {
        if (it->second) CloseHandle(it->second);
        submitEvents_.erase(it);
    }
}

bool ViGEmAdapter::submitReport(uint32_t serial, const GamepadReport& report) {
    std::lock_guard<std::mutex> lk(busMtx_);
    if (busHandle_ == INVALID_HANDLE_VALUE) return false;

    // Convert GamepadReport → XUSB_REPORT (binary compatible)
    XUSB_REPORT rpt;
    static_assert(sizeof(GamepadReport) == sizeof(XUSB_REPORT),
                  "GamepadReport and XUSB_REPORT must match");
    std::memcpy(&rpt, &report, sizeof(rpt));

    HANDLE evt = nullptr;
    auto it = submitEvents_.find(serial);
    if (it != submitEvents_.end()) evt = it->second;

    if (evt != nullptr) { return submitReportFast(busHandle_, (unsigned long)serial, rpt, evt); }
    // Fallback (should not happen)
    return false;
}

bool ViGEmAdapter::isDriverInstalled() { return isVigemInstalled(); }
