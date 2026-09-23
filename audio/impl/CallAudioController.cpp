/*
 * Copyright (C) 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */
#define LOG_TAG "CallAudioController"

#include "core/default/CallAudioController.h"

#include <cerrno>
#include <chrono>
#include <utility>

#include <log/log.h>

namespace samsung::audio {
namespace {
constexpr const char* kInactive[] = {
    "vsid=297816064;call_state=1", "vsid=299651072;call_state=1",
};
constexpr const char* kActive[] = {
    "vsid=297816064;call_state=2", "vsid=299651072;call_state=2",
};
constexpr const char* kSlot[] = {"g_call_sim_slot=0x01", "g_call_sim_slot=0x02"};
}  // namespace

CallAudioController::CallAudioController(SetMode setMode, SetParameters setParameters,
                                         ReadState readState)
    : mSetMode(std::move(setMode)), mSetParameters(std::move(setParameters)),
      mReadState(std::move(readState)) {}

CallAudioController::~CallAudioController() {
    stop();
}

int CallAudioController::setMode(int mode, bool inCall) {
    std::lock_guard<std::mutex> lock(mMutex);
    if (mStopped) return -ENODEV;
    if (!inCall && mInCall) {
        mInCall = false;
        blockCurrentSnapshot();
        applySlot(-1);
    }
    int result = mSetMode(mode);
    if (result == 0 && inCall) {
        mInCall = true;
        reconcile();
        if (!mThread.joinable()) mThread = std::thread(&CallAudioController::run, this);
    }
    mCondition.notify_all();
    return result;
}

void CallAudioController::stop() {
    {
        std::lock_guard<std::mutex> lock(mMutex);
        if (mStopped) return;
        mStopped = true;
        mInCall = false;
        applySlot(-1);
    }
    mCondition.notify_all();
    if (mThread.joinable()) mThread.join();
}

void CallAudioController::blockCurrentSnapshot() {
    ril::CallSnapshot snapshot;
    mBlocked = ril::CallSnapshot::parse(mReadState(), &snapshot) ? snapshot : mLast;
}

void CallAudioController::reconcile() {
    ril::CallSnapshot snapshot;
    if (!ril::CallSnapshot::parse(mReadState(), &snapshot)) {
        // An absent/malformed snapshot is not an authoritative empty call list.
        return;
    }
    if (snapshot.generation == mBlocked.generation && snapshot.sequence <= mBlocked.sequence) {
        return;
    }
    mLast = snapshot;
    applySlot(snapshot.selectSlot(mSlot));
}

void CallAudioController::applySlot(int slot) {
    if (mSlot == slot) return;
    bool ok = true;
    if (mSlot != -1) {
        // Always clear both sessions, including a partially applied change.
        ok = mSetParameters("g_call_state=1") == 0;
        for (const char* parameters : kInactive) {
            if (mSetParameters(parameters) != 0) ok = false;
        }
    }
    if (ok && slot >= 0) {
        ok = mSetParameters(kSlot[slot]) == 0;
        if (ok) ok = mSetParameters("g_call_state=2") == 0;
        if (ok) ok = mSetParameters(kActive[slot]) == 0;
    }
    if (ok) {
        ALOGI("CS voice session slot %d -> %d", mSlot, slot);
        mSlot = slot;
        mFailureLogged = false;
    } else {
        mSlot = -2;
        if (!mFailureLogged) ALOGW("Unable to apply CS voice session; retrying during IN_CALL");
        mFailureLogged = true;
    }
}

void CallAudioController::run() {
    std::unique_lock<std::mutex> lock(mMutex);
    while (!mStopped) {
        mCondition.wait(lock, [this] { return mStopped || mInCall; });
        if (mStopped) break;
        // Poll only the local snapshot, never the modem, and only during a CS
        // call. setMode/teardown and all HAL operations share this lock.
        if (!mCondition.wait_for(lock, std::chrono::milliseconds(50),
                                 [this] { return mStopped || !mInCall; })) {
            reconcile();
        }
    }
}

}  // namespace samsung::audio
