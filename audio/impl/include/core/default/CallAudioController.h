/*
 * Copyright (C) 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include <samsung/CallState.h>

namespace samsung::audio {

// Owned by Device, which also owns the underlying HAL. This remains valid
// after IPrimaryDevice::getDevice() and stops before IDevice::close().
class CallAudioController {
  public:
    using SetMode = std::function<int(int)>;
    using SetParameters = std::function<int(const char*)>;
    using ReadState = std::function<std::string()>;

    CallAudioController(SetMode setMode, SetParameters setParameters, ReadState readState);
    ~CallAudioController();
    int setMode(int mode, bool inCall);
    void stop();

  private:
    void run();
    void reconcile();
    void applySlot(int slot);
    void blockCurrentSnapshot();

    const SetMode mSetMode;
    const SetParameters mSetParameters;
    const ReadState mReadState;
    std::mutex mMutex;
    std::condition_variable mCondition;
    std::thread mThread;
    bool mStopped = false;
    bool mInCall = false;
    bool mFailureLogged = false;
    int mSlot = -1;  // -2 means a partial HAL operation must be retried.
    ril::CallSnapshot mLast{};
    ril::CallSnapshot mBlocked{};
};

}  // namespace samsung::audio
