// SPDX-License-Identifier: MIT

// The bytes of Nintendo R&D1's driver that a rip keeps whole.

#pragma once

#include <vector>

#include "agblib/agb/rom.h"
#include "agblib/rd1/driver.h"

#include "rip/driver.h"

namespace gsf::rip
{

// The runs of bytes that the library keeps whole once a run reads any of them: each instrument record that a program of
// a bank plays, directly or through a drum kit or a key split, the header of each sample that those records play, and
// the sample's points, up to the longer of its length and its loop's end, one byte a point. The records' sizes follow
// agblib's layout: 32 bytes for a sample, 36 for a PSG channel, 8 for a drum kit and 12 for a key split.
std::vector<RomSpan> Rd1WholeSpans(const agblib::agb::Rom& rom, const agblib::rd1::DriverInfo& info);

} // namespace gsf::rip
