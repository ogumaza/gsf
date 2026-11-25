// SPDX-License-Identifier: MIT

// The stub of a set: the ARM code that replaces the start-up code of the game. The first word of the cartridge branches
// to it. The stub sets up the stacks and the interrupt handler. It makes the calls of the start-up code that set up the
// sound driver, and starts the song whose number the mini stores. It then runs the driver as the game does: the
// routines that the interrupt handler of the game calls, and those that the main loop calls after each vertical blank.
//
// The code of the stub follows the ARM Architecture Reference Manual and the description of the GBA BIOS in GBATEK. The
// BIOS calls the interrupt handler of the stub through the word at 0x03007FFC. The handler acknowledges each interrupt
// in IF and in the BIOS flags at 0x03007FF8. VBlankIntrWait waits on those flags.

#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace gsf::rip
{

// The bits of IE and IF of the interrupts that the drivers use.
constexpr uint16_t kIrqVblank = 0x0001;
constexpr uint16_t kIrqVcount = 0x0004;
constexpr uint16_t kIrqTimer1 = 0x0010;
constexpr uint16_t kIrqDma1 = 0x0200;
constexpr uint16_t kIrqDma2 = 0x0400;

// The stacks that the BIOS gives each mode, by GBATEK.
constexpr uint32_t kIrqStack = 0x03007FA0;
constexpr uint32_t kSupervisorStack = 0x03007FE0;
constexpr uint32_t kSystemStack = 0x03007F00;

// The words of the BIOS in the work memory: the flags that IntrWait waits on, and the address of the interrupt handler.
constexpr uint32_t kBiosIrqFlags = 0x03007FF8;
constexpr uint32_t kIrqHandler = 0x03007FFC;

// Where a mini stores the number of the song for the stub to read: the last two bytes of the cartridge header. They're
// reserved, and outside the complement check of the header (GBATEK, "Cartridge Header"). The high byte of the address
// is that of the entry point, as the GSF description asks, whatever the size of the image. The number has 16 bits.
constexpr uint32_t kSongAddress = 0x080000BE;
constexpr uint32_t kMaxSong = 0xFFFF;

// A value that the stub passes to a routine or stores: a constant, plus the number of the song when `song` is set. With
// a table, the value is the word of the table at the number of the song instead. The stub places the tables after its
// literal pool.
struct Value
{
    uint32_t constant = 0;
    bool song = false;
    std::vector<uint32_t> table = {};
};

// The value that `table` contains for the song.
Value SongTable(std::vector<uint32_t> table);

// A constant value for each of `words`, in order.
std::vector<Value> Constants(std::span<const uint32_t> words);

// One thing that the stub does: a call of a routine (Thumb when bit 0 of its address is set) with up to four arguments,
// a store of 1, 2 or 4 bytes, or a copy of `size` bytes. A call can take its routine from a table indexed by the song's
// number. Its routines are then Thumb code, and an entry of 0 skips the call.
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

// What the stub runs for an interrupt. The model of the console has no interrupts. Each frame, the model runs the
// handlers in the order of StubPlan::handlers: a handler of the vertical blank once, and any other `per_frame` times on
// average.
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

    // The interrupts that the stub turns on in IE, besides any that `start` turned on. The interrupt of the vertical
    // blank also requires its bit in DISPSTAT. The stub sets that bit.
    uint16_t interrupts = kIrqVblank;

    // What the interrupt handler runs, for each interrupt, in this order.
    std::vector<InterruptHandler> handlers;

    // What the main loop runs after each vertical blank.
    std::vector<Operation> frame;

    // The interrupt handler of the game, or 0 for the handler of the stub. With a handler of the game, the stub stores
    // it at kIrqHandler in place of the stub handler. `start` then sets up what the handler of the game calls.
    // `handlers` then serves only the model: what each interrupt runs. A driver requires the handler of the game when
    // its interrupt routine returns through registers that only that handler sets, and turns the interrupts on again
    // inside.
    uint32_t game_handler = 0;
};

// A stub assembled at its address.
struct Stub
{
    uint32_t address = 0; // the first instruction of the stub: the target of the first word of the cartridge
    uint32_t loop = 0;    // the first instruction of the main loop: the wait for the next interrupt
    std::vector<uint8_t> code;
};

// The bytes of the stub for `plan`: the room to find for it.
uint32_t StubSize(const StubPlan& plan);

// Assembles `plan` at `address`, a multiple of 4. The stub reads the song's number at kSongAddress.
Stub BuildStub(const StubPlan& plan, uint32_t address);

// The ARM branch in the first word of the cartridge: from `from` to `to`.
uint32_t BranchWord(uint32_t from, uint32_t to);

} // namespace gsf::rip
