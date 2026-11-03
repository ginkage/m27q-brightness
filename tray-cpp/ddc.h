#pragma once

#include <windows.h>
#include <physicalmonitorenumerationapi.h>
#include <lowlevelmonitorconfigurationapi.h>

#include <string>
#include <utility>
#include <vector>

namespace ddc {

constexpr BYTE kVcpLuminance = 0x10;

// Measured floor for this panel: below 60 ms between transactions writes stop
// landing reliably. Only the verified path pays it.
constexpr int kSettleMs = 60;
constexpr int kMaxAttempts = 8;

// One DDC/CI display. Holds nothing but a handle and a name; the PHYSICAL_MONITOR
// array it came from is owned by DisplaySet, which frees it exactly once.
class Display {
public:
    Display(HANDLE handle, std::wstring name)
        : handle_(handle), name_(std::move(name)) {}

    const std::wstring& name() const { return name_; }
    DWORD max() const { return max_; }

    // One transaction, no retry. Blocks ~35-60 ms inside dxva2.
    bool TryReadOnce(DWORD* current, DWORD* maximum) {
        MC_VCP_CODE_TYPE type{};
        DWORD cur = 0, mx = 0;
        if (!GetVCPFeatureAndVCPFeatureReply(handle_, kVcpLuminance, &type, &cur, &mx))
            return false;
        if (mx > 0) max_ = mx;
        if (current) *current = cur;
        if (maximum) *maximum = mx;
        return true;
    }

    bool TryRead(DWORD* current, DWORD* maximum) {
        for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
            if (TryReadOnce(current, maximum)) {
                Sleep(kSettleMs);
                return true;
            }
            Sleep(kSettleMs);
        }
        return false;
    }

    // Fire and forget, used while the user is actively dragging: the call already
    // self-paces at ~35 ms and a stale value is about to be overwritten anyway.
    void WriteUnverified(DWORD value) {
        SetVCPFeature(handle_, kVcpLuminance, value);
    }

    // A true return from SetVCPFeature does not mean the write landed on this
    // panel, so it is confirmed by reading the value back.
    bool WriteVerified(DWORD value) {
        for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
            SetVCPFeature(handle_, kVcpLuminance, value);
            Sleep(kSettleMs);
            DWORD cur = 0;
            if (TryReadOnce(&cur, nullptr) && cur == value)
                return true;
            Sleep(kSettleMs);
        }
        return false;
    }

private:
    HANDLE handle_;
    std::wstring name_;
    DWORD max_ = 100;
};

// Owns the PHYSICAL_MONITOR arrays handed out by the OS. Each array goes back to
// DestroyPhysicalMonitors exactly once even when it describes several displays.
class DisplaySet {
public:
    DisplaySet() = default;
    DisplaySet(const DisplaySet&) = delete;
    DisplaySet& operator=(const DisplaySet&) = delete;
    DisplaySet(DisplaySet&&) noexcept = default;
    DisplaySet& operator=(DisplaySet&&) noexcept = default;

    ~DisplaySet() { Close(); }

    void Close() {
        for (auto& arr : arrays_)
            if (!arr.empty())
                DestroyPhysicalMonitors(static_cast<DWORD>(arr.size()), arr.data());
        arrays_.clear();
        displays_.clear();
    }

    std::vector<Display>& displays() { return displays_; }
    const std::vector<Display>& displays() const { return displays_; }

    static DisplaySet Open() {
        std::vector<HMONITOR> hmonitors;
        EnumDisplayMonitors(nullptr, nullptr, &DisplaySet::EnumProc,
                            reinterpret_cast<LPARAM>(&hmonitors));

        DisplaySet set;
        for (HMONITOR hmon : hmonitors) {
            DWORD count = 0;
            if (!GetNumberOfPhysicalMonitorsFromHMONITOR(hmon, &count) || count == 0)
                continue;

            std::vector<PHYSICAL_MONITOR> arr(count);
            if (!GetPhysicalMonitorsFromHMONITOR(hmon, count, arr.data()))
                continue;

            for (const auto& pm : arr)
                set.displays_.emplace_back(pm.hPhysicalMonitor,
                                           std::wstring(pm.szPhysicalMonitorDescription));
            set.arrays_.push_back(std::move(arr));
        }
        return set;
    }

private:
    static BOOL CALLBACK EnumProc(HMONITOR h, HDC, LPRECT, LPARAM lp) {
        reinterpret_cast<std::vector<HMONITOR>*>(lp)->push_back(h);
        return TRUE;
    }

    std::vector<std::vector<PHYSICAL_MONITOR>> arrays_;
    std::vector<Display> displays_;
};

}  // namespace ddc
