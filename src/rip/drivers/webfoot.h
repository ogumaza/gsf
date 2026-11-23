// SPDX-License-Identifier: MIT

// The value of WAITCNT that the stub of Webfoot Technologies' driver sets for a game that the table of gsf doesn't
// name.

#pragma once

#include <cstdint>

#include "agblib/agb/rom.h"

namespace gsf::rip
{

// Returns the value that the crt0 of the program at `program` stores in WAITCNT, or 0 where it stores none. The crt0 is
// ARM code at 0xC0 bytes into the program, and the search reads its first 0x100 bytes. The crt0 sets a register to the
// address of WAITCNT or to the I/O base 0x04000000 (`mov` or a literal), and another to the value from a literal. It
// then stores the value: `str rD, [rN, #0x204]` or `str rD, [rN]` (0x080000DC to 0x080000E0 in The Legacy of Goku II).
// A register that another instruction writes no longer counts.
uint16_t Crt0WaitCnt(const agblib::agb::Rom& rom, uint32_t program);

} // namespace gsf::rip
