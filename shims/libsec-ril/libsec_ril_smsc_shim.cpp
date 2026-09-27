// SPDX-License-Identifier: Apache-2.0
//
// Wrap Samsung libsec-ril.so without replacing its private callback table.

#include <dlfcn.h>

#include <cstddef>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

#include "SmscAddress.h"

#include <log/log.h>
#include <telephony/ril.h>

#ifndef REAL_LIB_NAME
#error "REAL_LIB_NAME must be defined by Android.bp"
#endif

#ifndef RIL_REQUEST_SEND_SMS
#define RIL_REQUEST_SEND_SMS 25
#endif

#ifndef RIL_REQUEST_SEND_SMS_EXPECT_MORE
#define RIL_REQUEST_SEND_SMS_EXPECT_MORE 26
#endif

static constexpr char kRealPath[] = "/vendor/lib64/" REAL_LIB_NAME;

using SamsungRequestFunc = void (*)(
        int, void*, size_t, RIL_Token, RIL_SOCKET_ID);

// Samsung extends RIL_RadioFunctions after onRequest. Only describe the
// common prefix so the private callbacks remain in the real table.
struct SamsungRilFunctionsPrefix {
    int version;
    SamsungRequestFunc onRequest;
};

static_assert(
        offsetof(SamsungRilFunctionsPrefix, onRequest) == sizeof(void*),
        "unexpected Samsung RIL function-table prefix");

using RilInit = const RIL_RadioFunctions* (*)(
        const RIL_Env*, int, char**);

static void* gRealHandle = nullptr;
static SamsungRequestFunc gRealOnRequest = nullptr;

// The shipped Android 33 rild passes exactly these four callbacks (32 bytes).
// Keep the original timed callback, unsolicited callback and request ACK.
static_assert(sizeof(RIL_Env) == 4 * sizeof(void*));
static RIL_Env gShimEnv;
static const RIL_Env* gRealEnv = nullptr;

struct PendingRequest {
    int request;
    RIL_SOCKET_ID socket;
};
static std::mutex gRequestMutex;
static std::unordered_map<RIL_Token, PendingRequest> gRequests;

static void shimOnRequestComplete(RIL_Token token, RIL_Errno error,
                                  void* response, size_t responselen) {
    PendingRequest pending{};
    {
        std::lock_guard<std::mutex> lock(gRequestMutex);
        auto it = gRequests.find(token);
        if (it != gRequests.end()) {
            pending = it->second;
            gRequests.erase(it);
        }
    }

    if (pending.request == RIL_REQUEST_GET_SMSC_ADDRESS && error == RIL_E_SUCCESS) {
        std::string address;
        // SmsRespBuilder returns snprintf's byte count, excluding the NUL.
        // Accept both conventions without reading beyond the supplied buffer.
        if (response != nullptr && responselen > 0 && responselen <= 128) {
            const char* text = static_cast<const char*>(response);
            size_t length = strnlen(text, responselen);
            if (samsung::ril::normalizeSmscAddress({text, length}, &address)) {
                gRealEnv->OnRequestComplete(token, error, address.data(), address.size() + 1);
                return;
            }
        }
        ALOGW("sec-ril-shim: invalid SMSC response on socket %d", pending.socket);
        gRealEnv->OnRequestComplete(token, RIL_E_INVALID_RESPONSE, nullptr, 0);
        return;
    }
    gRealEnv->OnRequestComplete(token, error, response, responselen);
}

static void* getRealHandle() {
    if (gRealHandle != nullptr) {
        return gRealHandle;
    }

    ALOGI("sec-ril-smsc-shim: loading %s", kRealPath);
    gRealHandle = dlopen(kRealPath, RTLD_NOW);
    if (gRealHandle == nullptr) {
        ALOGE("sec-ril-smsc-shim: dlopen failed: %s", dlerror());
    }

    return gRealHandle;
}

static RilInit getRealInit(const char* name) {
    void* realHandle = getRealHandle();
    if (realHandle == nullptr) {
        return nullptr;
    }

    dlerror();
    auto realInit = reinterpret_cast<RilInit>(dlsym(realHandle, name));
    const char* error = dlerror();
    if (error != nullptr) {
        ALOGE("sec-ril-smsc-shim: dlsym %s failed: %s", name, error);
        return nullptr;
    }

    return realInit;
}

static bool isCsSmsRequest(int request) {
    return request == RIL_REQUEST_SEND_SMS
            || request == RIL_REQUEST_SEND_SMS_EXPECT_MORE;
}

static void shimOnRequest(
        int request, void* data, size_t datalen, RIL_Token token,
        RIL_SOCKET_ID socketId) {
    if (request == RIL_REQUEST_GET_SMSC_ADDRESS) {
        std::lock_guard<std::mutex> lock(gRequestMutex);
        gRequests[token] = {request, socketId};
    }

    // Only 3GPP SMS has an SCA. Preserve CDMA, retry metadata and the TPDU.
    char** sms = nullptr;
    RIL_IMS_SMS_Message ims{};
    if (isCsSmsRequest(request) && data != nullptr && datalen == 2 * sizeof(char*)) {
        sms = static_cast<char**>(data);
    } else if (request == RIL_REQUEST_IMS_SEND_SMS && data != nullptr &&
               datalen == sizeof(ims)) {
        ims = *static_cast<RIL_IMS_SMS_Message*>(data);
        if (ims.tech == RADIO_TECH_3GPP) sms = ims.message.gsmMessage;
    }
    if (sms != nullptr && sms[0] != nullptr && sms[1] != nullptr) {
        std::string pdu;
        size_t length = strnlen(sms[0], 129);
        if (length > 128 || !samsung::ril::normalizeSmscPdu({sms[0], length}, &pdu)) {
            ALOGW("sec-ril-shim: invalid SMSC for request %d on socket %d", request, socketId);
            gRealEnv->OnRequestComplete(token, RIL_E_INVALID_ARGUMENTS, nullptr, 0);
            return;
        }
        if (pdu != sms[0]) {
            char* fixed[] = {pdu.data(), sms[1]};
            if (request == RIL_REQUEST_IMS_SEND_SMS) {
                ims.message.gsmMessage = fixed;
                gRealOnRequest(request, &ims, sizeof(ims), token, socketId);
            } else {
                gRealOnRequest(request, fixed, sizeof(fixed), token, socketId);
            }
            return;
        }
    }
    gRealOnRequest(request, data, datalen, token, socketId);
}

extern "C" const RIL_RadioFunctions* RIL_Init(
        const RIL_Env* env, int argc, char** argv) {
    RilInit realRilInit = getRealInit("RIL_Init");
    if (realRilInit == nullptr) {
        return nullptr;
    }

    if (env == nullptr || env->OnRequestComplete == nullptr) return nullptr;
    gRealEnv = env;
    gShimEnv = *env;
    gShimEnv.OnRequestComplete = shimOnRequestComplete;
    const RIL_RadioFunctions* real = realRilInit(&gShimEnv, argc, argv);
    if (real == nullptr) {
        ALOGE("sec-ril-smsc-shim: invalid real RIL function table");
        return real;
    }

    auto* realPrefix = reinterpret_cast<SamsungRilFunctionsPrefix*>(
            const_cast<RIL_RadioFunctions*>(real));
    if (realPrefix->onRequest == nullptr) {
        ALOGE("sec-ril-smsc-shim: real onRequest is null");
        return real;
    }

    if (realPrefix->onRequest != shimOnRequest) {
        gRealOnRequest = realPrefix->onRequest;
        realPrefix->onRequest = shimOnRequest;
    } else if (gRealOnRequest == nullptr) {
        ALOGE("sec-ril-smsc-shim: missing saved real onRequest");
        return nullptr;
    }

    ALOGI("sec-ril-smsc-shim: installed SMSC normalization shim");
    return real;
}

extern "C" const RIL_RadioFunctions* RIL_SAP_Init(
        const RIL_Env* env, int argc, char** argv) {
    RilInit realSapInit = getRealInit("RIL_SAP_Init");
    if (realSapInit == nullptr) {
        return nullptr;
    }

    return realSapInit(env, argc, argv);
}
