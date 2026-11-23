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
// the points of the sample (agblib's PointsOfSample()). agblib's RecordBytes() returns the size of each record.
std::vector<RomSpan> Rd1WholeSpans(const agblib::agb::Rom& rom, const agblib::rd1::DriverInfo& info);

} // namespace gsf::rip
