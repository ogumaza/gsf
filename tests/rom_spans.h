// SPDX-License-Identifier: MIT

// The check of a driver's whole spans (rip/driver.h) that the drivers' tests share.

#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

#include "rip/driver.h"

namespace gsf::testing
{

// Whether `spans` has the span of `size` bytes at `address`.
inline bool HasSpan(const std::vector<gsf::rip::RomSpan>& spans, uint32_t address, uint32_t size)
{
    return std::any_of(spans.begin(), spans.end(),
                       [&](const gsf::rip::RomSpan& span) { return span.address == address && span.size == size; });
}

} // namespace gsf::testing
