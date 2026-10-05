/*
 * Copyright (C) 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <string>
#include <string_view>

namespace samsung::ril {

// GET returns a dialable address; SEND takes a length-prefixed BCD SCA.
// Neither function invents a service center when the input is invalid.
bool normalizeSmscAddress(std::string_view input, std::string* address);
bool normalizeSmscPdu(std::string_view input, std::string* pdu);

}  // namespace samsung::ril
