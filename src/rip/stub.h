// SPDX-License-Identifier: MIT

// A set's stub: the ARM code that takes the place of the game's start-up code. The cartridge's first word branches to
// it. It sets up the stacks and the interrupt handler, makes the calls of the game's start-up code that set up the
// sound driver, starts the song whose number the mini stores, and then runs the driver as the game does: the routines
// that the game's interrupt handler calls, and those that its main loop calls after each vertical blank.
//
// The stub's code follows ARM's architecture reference manual and GBATEK's description of the GBA's BIOS: its
// interrupt handler is called from the BIOS's, through the word at 0x03007FFC, and acknowledges each interrupt in IF
// and in the BIOS's flags at 0x03007FF8, which VBlankIntrWait waits on.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace gsf::rip
{

// The interrupts, as bits of IE and IF.
constexpr uint16_t kIrqVblank = 0x0001;
constexpr uint16_t kIrqTimer0 = 0x0008;
constexpr uint16_t kIrqTimer1 = 0x0010;
constexpr uint16_t kIrqTimer2 = 0x0020;
constexpr uint16_t kIrqTimer3 = 0x0040;
constexpr uint16_t kIrqDma0 = 0x0100;
constexpr uint16_t kIrqDma1 = 0x0200;
constexpr uint16_t kIrqDma2 = 0x0400;
constexpr uint16_t kIrqDma3 = 0x0800;

// The stacks that the BIOS gives each mode, by GBATEK.
constexpr uint32_t kIrqStack = 0x03007FA0;
constexpr uint32_t kSupervisorStack = 0x03007FE0;
constexpr uint32_t kSystemStack = 0x03007F00;

// The BIOS's words in the work memory: the flags that IntrWait waits on, and the address of the game's handler.
constexpr uint32_t kBiosIrqFlags = 0x03007FF8;
constexpr uint32_t kIrqHandler = 0x03007FFC;

// Where a mini stores the song's number, which the stub reads: the last two bytes of the cartridge's header, reserved
// and outside the header's complement check (GBATEK, "Cartridge Header"). The address's high byte is the entry
// point's, as the GSF description asks, whatever the image's size. The number has 16 bits.
constexpr uint32_t kSongAddress = 0x080000BE;
constexpr uint32_t kMaxSong = 0xFFFF;

// A value that the stub passes to a routine or stores: a constant, plus the song's number when `song` is set. With a
// table, the value is the table's word at the song's number instead. The stub keeps the tables after its literal
// pool.
struct Value
{
    uint32_t constant = 0;
    bool song = false;
    std::vector<uint32_t> table = {};
};

// The value that `table` holds for the song.
Value SongTable(std::vector<uint32_t> table);

// One thing that the stub does: a call of a routine (Thumb when bit 0 of its address is set) with up to four
// arguments, a store of 1, 2 or 4 bytes, or a copy of `size` bytes. A call can take its routine from a table indexed
// by the song's number. Its routines are then Thumb code, and an entry of 0 skips the call.
struct Operation
{
    enum class Kind
    {
        kCall,
        kStore,
        kCopy,
    };

    static Operation Call(uint32_t routine, std::vector<Value> args = {});
    static Operation CallFromTable(std::vector<uint32_t> routines, std::vector<Value> args = {});
    static Operation Store(uint32_t address, Value value, int size);
    static Operation Copy(uint32_t source, uint32_t destination, uint32_t size);

    Kind kind = Kind::kCall;
    uint32_t address = 0; // the routine, the store's address or the copy's source
    uint32_t destination = 0;
    uint32_t size = 0;
    std::vector<Value> values;      // a call's arguments, or a store's value
    std::vector<uint32_t> routines; // a call's routine for each song, in place of `address`
};

// What the stub runs for an interrupt. The model of the console has no interrupts. Its runs of a plan run a handler
// `per_frame` times a frame on average, after the vertical blank's.
struct InterruptHandler
{
    uint16_t interrupt = kIrqVblank; // one bit
    std::vector<Operation> operations;
    double per_frame = 1.0;
};

// What a driver's stub does, in the game's order.
struct StubPlan
{
    // Before the interrupts are on: the game's set-up of the driver, then the start of the song.
    std::vector<Operation> start;

    // The interrupts that the stub turns on in IE, besides any that `start` turned on. The vertical blank's needs
    // DISPSTAT's bit too, which the stub sets.
    uint16_t interrupts = kIrqVblank;

    // What the interrupt handler runs, for each interrupt, in this order.
    std::vector<InterruptHandler> handlers;

    // What the main loop runs after each vertical blank.
    std::vector<Operation> frame;
};

// A stub assembled at its address.
struct Stub
{
    uint32_t address = 0; // the stub's first instruction, which the cartridge's first word branches to
    uint32_t loop = 0;    // the main loop's first instruction: its wait for the next interrupt
    std::vector<uint8_t> code;
};

// The bytes of the stub for `plan`: the room to find for it.
uint32_t StubSize(const StubPlan& plan);

// Assembles `plan` at `address`, a multiple of 4. The stub reads the song's number at kSongAddress.
Stub BuildStub(const StubPlan& plan, uint32_t address);

// The ARM branch that the cartridge's first word holds: from `from` to `to`.
uint32_t BranchWord(uint32_t from, uint32_t to);

} // namespace gsf::rip
