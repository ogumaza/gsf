// SPDX-License-Identifier: MIT

// Tests of the stub (rip/stub.h) and of the model's runs of its plan (rip/model.h). The stub runs on ntdlib's ARM
// interpreter, with routines that record their calls in the work memory.

#include "rip/stub.h"

#include <cstdint>
#include <iterator>
#include <vector>

#include "agblib/agb/rom.h"
#include "agblib/agb/system.h"
#include "ntdlib/arm/assembler.h"
#include "ntdlib/arm/cpu.h"
#include "xsflib/testing/check.h"

#include "rip/model.h"

namespace
{

using gsf::rip::InterruptHandler;
using gsf::rip::Operation;
using gsf::rip::StubPlan;
using gsf::rip::Value;

constexpr uint32_t kRomBase = 0x08000000;
constexpr uint32_t kEwram = 0x02000000;

// The test's routines, in ARM. Record stores r0 and r1 at kRecorded. Each count adds 1 to its word.
constexpr uint32_t kRecord = kRomBase + 0x200;
constexpr uint32_t kCountVblank = kRomBase + 0x240;
constexpr uint32_t kCountTimer = kRomBase + 0x280;
constexpr uint32_t kCountFrame = kRomBase + 0x2C0;
constexpr uint32_t kData = kRomBase + 0x300; // the bytes 1 to 16

// A Thumb routine that stores r0 at kThumbStored and returns with pop {pc}, which stays in Thumb state.
constexpr uint32_t kThumbStore = kRomBase + 0x380;
constexpr uint32_t kStubAddress = kRomBase + 0x400;
constexpr uint32_t kImageSize = 0x800;

constexpr uint32_t kRecorded = kEwram + 0x100;
constexpr uint32_t kVblanks = kEwram + 0x110;
constexpr uint32_t kTimers = kEwram + 0x114;
constexpr uint32_t kFrames = kEwram + 0x118;
constexpr uint32_t kThumbStored = kEwram + 0x11C;

constexpr uint32_t kSong = 3;

void Place(std::vector<uint8_t>& image, uint32_t address, const std::vector<uint8_t>& code)
{
    for (size_t i = 0; i < code.size(); i++)
    {
        image.at(address - kRomBase + i) = code[i];
    }
}

std::vector<uint8_t> Counter(uint32_t address, uint32_t word)
{
    ntdlib::arm::ArmAssembler a(address);
    a.LoadConstant(12, word);
    a.Ldr(1, 12);
    a.Add(1, 1, 1);
    a.Str(1, 12);
    a.Bx(ntdlib::arm::kLr);
    return a.Finish();
}

std::vector<uint8_t> TestImage()
{
    std::vector<uint8_t> image(kImageSize);

    ntdlib::arm::ArmAssembler record(kRecord);
    record.LoadConstant(12, kRecorded);
    record.Str(0, 12);
    record.Str(1, 12, 4);
    record.Bx(ntdlib::arm::kLr);
    Place(image, kRecord, record.Finish());

    Place(image, kCountVblank, Counter(kCountVblank, kVblanks));
    Place(image, kCountTimer, Counter(kCountTimer, kTimers));
    Place(image, kCountFrame, Counter(kCountFrame, kFrames));
    for (uint32_t i = 0; i < 16; i++)
    {
        image[kData - kRomBase + i] = uint8_t(i + 1);
    }

    // push {lr}; ldr r1, [pc, #4]; str r0, [r1]; pop {pc}; then the word.
    const uint16_t thumb[] = {0xB500, 0x4901, 0x6008, 0xBD00, uint16_t(kThumbStored), uint16_t(kThumbStored >> 16)};
    for (size_t i = 0; i < std::size(thumb); i++)
    {
        image[kThumbStore - kRomBase + 2 * i] = uint8_t(thumb[i]);
        image[kThumbStore - kRomBase + 2 * i + 1] = uint8_t(thumb[i] >> 8);
    }

    return image;
}

// Two stores, the second from a table, a copy of words and one of bytes, a Thumb call, two calls whose routines come
// from a table, the second skipped, and a call with the song's number. The vertical blank and timer 1 count.
StubPlan TestPlan()
{
    StubPlan plan;
    plan.start = {
        Operation::Store(kEwram + 0x200, {7}, 4),
        Operation::Store(kEwram + 0x204, gsf::rip::SongTable({10, 20, 30, 40}), 2),
        Operation::Copy(kData, kEwram + 0x300, 8),
        Operation::Copy(kData + 1, kEwram + 0x310, 3),
        Operation::Call(kThumbStore | 1, {{0, true}}),
        Operation::CallFromTable({0, 0, 0, kThumbStore | 1}, {{100, true}}),
        Operation::CallFromTable({kThumbStore | 1, 0, 0, 0}, {{200}}),
        Operation::Call(kRecord, {{5}, {1, true}}),
    };
    plan.interrupts = gsf::rip::kIrqVblank | gsf::rip::kIrqTimer1;
    plan.handlers = {
        {gsf::rip::kIrqVblank, {Operation::Call(kCountVblank)}},
        {gsf::rip::kIrqTimer1, {Operation::Call(kCountTimer)}, 2.5},
    };
    plan.frame = {Operation::Call(kCountFrame)};
    return plan;
}

agblib::agb::Rom RomWith(const std::vector<uint8_t>& image)
{
    agblib::agb::Rom rom;
    rom.Assign(image);
    return rom;
}

void TestStart()
{
    std::vector<uint8_t> image = TestImage();
    const gsf::rip::Stub stub = gsf::rip::BuildStub(TestPlan(), kStubAddress, kSong);
    Place(image, kStubAddress, stub.code);
    const agblib::agb::Rom rom = RomWith(image);
    agblib::agb::System system(rom);
    ntdlib::arm::Cpu& cpu = system.Cpu();

    // The stub runs its start up to the main loop's wait.
    cpu.Jump(stub.address, ntdlib::arm::Mode::kSystem);
    cpu.SetStopAddress(stub.loop);
    const ntdlib::arm::StopReason reason = cpu.Run(1'000'000);

    XSFLIB_CHECK(reason == ntdlib::arm::StopReason::kStopAddress);
    XSFLIB_CHECK(stub.song_address > stub.loop && stub.song_address < stub.address + stub.code.size());
    XSFLIB_CHECK(system.Load(stub.song_address, 4) == kSong);

    // The operations, in order: the song's number is the stub's word.
    XSFLIB_CHECK(system.Load(kEwram + 0x200, 4) == 7);
    XSFLIB_CHECK(system.Load(kEwram + 0x204, 4) == 40);
    XSFLIB_CHECK(system.Load(kEwram + 0x300, 4) == 0x04030201 && system.Load(kEwram + 0x304, 4) == 0x08070605);
    XSFLIB_CHECK(system.Load(kEwram + 0x310, 4) == 0x00040302);
    XSFLIB_CHECK(system.Load(kThumbStored, 4) == 100 + kSong);
    XSFLIB_CHECK(system.Load(kRecorded, 4) == 5 && system.Load(kRecorded + 4, 4) == 1 + kSong);

    // The handler is in place, and the interrupts are on: DISPSTAT's VBlank bit, IE, IME and the CPSR. SOUNDBIAS is
    // the BIOS's.
    const uint32_t handler = system.Load(gsf::rip::kIrqHandler, 4);
    XSFLIB_CHECK(handler > stub.loop && handler < stub.song_address);
    XSFLIB_CHECK(system.Load(0x04000004, 2) == 0x0008);
    XSFLIB_CHECK(system.Load(0x04000200, 2) == 0x0011);
    XSFLIB_CHECK(system.Load(0x04000208, 2) == 1);
    XSFLIB_CHECK(system.Load(0x04000088, 2) == 0x0200);
    XSFLIB_CHECK((cpu.Cpsr() & 0xFF) == 0x1F);
    XSFLIB_CHECK(cpu.Reg(ntdlib::arm::kSp) == gsf::rip::kSystemStack);
}

void TestInterruptsAndLoop()
{
    std::vector<uint8_t> image = TestImage();
    const gsf::rip::Stub stub = gsf::rip::BuildStub(TestPlan(), kStubAddress, kSong);
    Place(image, kStubAddress, stub.code);
    const agblib::agb::Rom rom = RomWith(image);
    agblib::agb::System system(rom);
    ntdlib::arm::Cpu& cpu = system.Cpu();
    cpu.Jump(stub.address, ntdlib::arm::Mode::kSystem);
    cpu.SetStopAddress(stub.loop);
    cpu.Run(1'000'000);

    // The BIOS calls the handler in IRQ mode for a vertical blank and timer 1's interrupt, with timer 2's off in IE.
    system.Store(0x04000202, 2, 0x0031);
    cpu.SetCpsr(0x92);
    cpu.Call(system.Load(gsf::rip::kIrqHandler, 4), {}, 100'000);

    XSFLIB_CHECK(system.Load(kVblanks, 4) == 1 && system.Load(kTimers, 4) == 1);
    XSFLIB_CHECK(system.Load(0x04000202, 2) == 0x0011);
    XSFLIB_CHECK(system.Load(gsf::rip::kBiosIrqFlags, 2) == 0x0011);
    XSFLIB_CHECK((cpu.Cpsr() & 0xFF) == 0xD2);
    XSFLIB_CHECK(cpu.Reg(ntdlib::arm::kSp) == gsf::rip::kIrqStack);

    // The main loop waits with VBlankIntrWait, then runs the frame's operations.
    std::vector<uint32_t> calls;
    system.on_swi = [&](uint32_t number)
    {
        calls.push_back(number);
        if (calls.size() == 3)
        {
            cpu.Stop();
        }
        return true;
    };
    cpu.SetCpsr(0x1F);
    cpu.SetStopAddress(0xFFFFFFFF);
    const ntdlib::arm::StopReason reason = cpu.Run(1'000'000);

    XSFLIB_CHECK(reason == ntdlib::arm::StopReason::kStopped);
    XSFLIB_CHECK(calls == (std::vector<uint32_t>{5, 5, 5}));
    XSFLIB_CHECK(system.Load(kFrames, 4) == 2);
}

void TestModelRun()
{
    const agblib::agb::Rom rom = RomWith(TestImage());
    std::vector<uint8_t> reads;

    gsf::rip::ModelRun run(rom, TestPlan(), kSong, &reads);
    for (int i = 0; i < 3; i++)
    {
        run.Frame();
    }

    // Timer 1's handler runs 2.5 times a frame: 2, 3 and 2 times.
    XSFLIB_CHECK(run.Frames() == 3);
    XSFLIB_CHECK(run.Gba().Load(kRecorded + 4, 4) == 1 + kSong);
    XSFLIB_CHECK(run.Gba().Load(kThumbStored, 4) == 100 + kSong);
    XSFLIB_CHECK(run.Gba().Load(kEwram + 0x204, 4) == 40);
    XSFLIB_CHECK(run.Gba().Load(kEwram + 0x310, 4) == 0x00040302);
    XSFLIB_CHECK(run.Gba().Load(kVblanks, 4) == 3);
    XSFLIB_CHECK(run.Gba().Load(kTimers, 4) == 7);
    XSFLIB_CHECK(run.Gba().Load(kFrames, 4) == 3);

    // The reads cover the routines that ran and the bytes that the copies read, and nothing past the data.
    XSFLIB_REQUIRE(reads.size() >= kImageSize);
    XSFLIB_CHECK(reads[kRecord - kRomBase] && reads[kCountFrame - kRomBase]);
    XSFLIB_CHECK(reads[kData - kRomBase] && reads[kData + 7 - kRomBase] && !reads[kData + 8 - kRomBase]);
    XSFLIB_CHECK(!reads[kStubAddress - kRomBase]);
}

void TestStandIn()
{
    const agblib::agb::Rom rom = RomWith(TestImage());

    // A stand-in for timer 1's routine counts in its place.
    gsf::rip::ModelRun run(rom, TestPlan(), kSong, nullptr);
    int stood_in = 0;
    run.on_call = [&](agblib::agb::System&, uint32_t routine)
    {
        if (routine != kCountTimer)
        {
            return false;
        }

        stood_in++;
        return true;
    };
    run.Frame();
    run.Frame();

    XSFLIB_CHECK(stood_in == 5);
    XSFLIB_CHECK(run.Gba().Load(kTimers, 4) == 0);
}

void TestBranchWord()
{
    XSFLIB_CHECK(gsf::rip::BranchWord(kRomBase, kRomBase + 0xC0) == 0xEA00002E);
    XSFLIB_CHECK(gsf::rip::BranchWord(kRomBase, kRomBase + 0x01FFFF00) == 0xEA7FFFBE);
}

} // namespace

int main()
{
    TestStart();
    TestInterruptsAndLoop();
    TestModelRun();
    TestStandIn();
    TestBranchWord();

    return xsflib::testing::Summary("stub");
}
