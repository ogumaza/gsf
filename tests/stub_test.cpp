// SPDX-License-Identifier: MIT

// Tests of the stub (rip/stub.h) and of the model's runs of its plan (rip/model.h). The stub runs on ntdlib's ARM
// interpreter, with routines that record their calls in the work memory.

#include "rip/stub.h"

#include <cstdint>
#include <stdexcept>
#include <vector>

#include "agblib/agb/rom.h"
#include "agblib/agb/system.h"
#include "agblib/testing/agb_cart.h"
#include "ntdlib/arm/assembler.h"
#include "ntdlib/arm/cpu.h"
#include "xsflib/testing/check.h"

#include "rip/model.h"

namespace
{

using agblib::agb::kEwramBase;
using agblib::agb::kRomBase;
using agblib::testing::agb_files::CartImage;
using gsf::rip::InterruptHandler;
using gsf::rip::Operation;
using gsf::rip::StubPlan;
using gsf::rip::Value;

// The test's routines, in ARM. Record stores r0 and r1 at kRecorded. Each count adds 1 to its word.
constexpr uint32_t kRecord = kRomBase + 0x200;
constexpr uint32_t kCountVblank = kRomBase + 0x240;
constexpr uint32_t kCountTimer = kRomBase + 0x280;
constexpr uint32_t kCountFrame = kRomBase + 0x2C0;
constexpr uint32_t kData = kRomBase + 0x300; // the bytes 1 to 16

// A Thumb routine that stores r0 at kThumbStored and returns with pop {pc}. The return stays in Thumb state.
constexpr uint32_t kThumbStore = kRomBase + 0x380;
constexpr uint32_t kStubAddress = kRomBase + 0x400;
constexpr uint32_t kImageSize = 0x800;

constexpr uint32_t kRecorded = kEwramBase + 0x100;
constexpr uint32_t kVblanks = kEwramBase + 0x110;
constexpr uint32_t kTimers = kEwramBase + 0x114;
constexpr uint32_t kFrames = kEwramBase + 0x118;
constexpr uint32_t kThumbStored = kEwramBase + 0x11C;

constexpr uint32_t kSong = 3;

// A routine at `address` that adds 1 to the word at `word`.
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

// The test's image, with `song` where a mini stores the song's number.
CartImage TestImage(uint32_t song = kSong)
{
    CartImage image(kImageSize);
    image.Put16(gsf::rip::kSongAddress, uint16_t(song));

    ntdlib::arm::ArmAssembler record(kRecord);
    record.LoadConstant(12, kRecorded);
    record.Str(0, 12);
    record.Str(1, 12, 4);
    record.Bx(ntdlib::arm::kLr);
    image.PutBytes(kRecord, record.Finish());

    image.PutBytes(kCountVblank, Counter(kCountVblank, kVblanks));
    image.PutBytes(kCountTimer, Counter(kCountTimer, kTimers));
    image.PutBytes(kCountFrame, Counter(kCountFrame, kFrames));
    for (uint32_t i = 0; i < 16; i++)
    {
        image.Put8(kData + i, uint8_t(i + 1));
    }

    // push {lr}; ldr r1, [pc, #4]; str r0, [r1]; pop {pc}; then the word.
    image.PutHalfwords(kThumbStore,
                       {0xB500, 0x4901, 0x6008, 0xBD00, uint16_t(kThumbStored), uint16_t(kThumbStored >> 16)});

    return image;
}

// Two stores, the second from a table, a copy of words and one of bytes, a Thumb call, two calls whose routines come
// from a table, the second skipped, and a call with the song's number. The vertical blank and timer 1 count.
StubPlan TestPlan()
{
    StubPlan plan;
    plan.start = {
        Operation::Store(kEwramBase + 0x200, {7}, 4),
        Operation::Store(kEwramBase + 0x204, gsf::rip::SongTable({10, 20, 30, 40}), 2),
        Operation::Copy(kData, kEwramBase + 0x300, 8),
        Operation::Copy(kData + 1, kEwramBase + 0x310, 3),
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

// `image` with `stub` in place. Throws std::out_of_range if the stub runs past the image.
agblib::agb::Rom RomWithStub(CartImage image, const gsf::rip::Stub& stub)
{
    if (stub.address - kRomBase + stub.code.size() > kImageSize)
    {
        throw std::out_of_range("the stub runs past the test's image");
    }

    image.PutBytes(stub.address, stub.code);

    return image.ToRom();
}

// Runs the stub's start on `system`, in system mode, up to the main loop's wait.
ntdlib::arm::StopReason RunStart(agblib::agb::System& system, const gsf::rip::Stub& stub)
{
    ntdlib::arm::Cpu& cpu = system.Cpu();
    cpu.Jump(stub.address, ntdlib::arm::Mode::kSystem);
    cpu.SetStopAddress(stub.loop);

    return cpu.Run(1'000'000);
}

void TestStart()
{
    const gsf::rip::Stub stub = gsf::rip::BuildStub(TestPlan(), kStubAddress);
    const agblib::agb::Rom rom = RomWithStub(TestImage(), stub);
    agblib::agb::System system(rom);

    const ntdlib::arm::StopReason reason = RunStart(system, stub);

    XSFLIB_CHECK(reason == ntdlib::arm::StopReason::kStopAddress);

    // The operations, in order: the song's number is the header's halfword at kSongAddress.
    XSFLIB_CHECK(system.Load(kEwramBase + 0x200, 4) == 7);
    XSFLIB_CHECK(system.Load(kEwramBase + 0x204, 4) == 40);
    XSFLIB_CHECK(system.Load(kEwramBase + 0x300, 4) == 0x04030201 && system.Load(kEwramBase + 0x304, 4) == 0x08070605);
    XSFLIB_CHECK(system.Load(kEwramBase + 0x310, 4) == 0x00040302);
    XSFLIB_CHECK(system.Load(kThumbStored, 4) == 100 + kSong);
    XSFLIB_CHECK(system.Load(kRecorded, 4) == 5 && system.Load(kRecorded + 4, 4) == 1 + kSong);

    // The handler is in place, and the interrupts are on: DISPSTAT's VBlank bit, IE, IME and the CPSR. SOUNDBIAS is
    // the BIOS's.
    const uint32_t handler = system.Load(gsf::rip::kIrqHandler, 4);
    XSFLIB_CHECK(handler > stub.loop && handler < stub.address + stub.code.size());
    XSFLIB_CHECK(system.Load(0x04000004, 2) == 0x0008);
    XSFLIB_CHECK(system.Load(0x04000200, 2) == 0x0011);
    XSFLIB_CHECK(system.Load(0x04000208, 2) == 1);
    XSFLIB_CHECK(system.Load(agblib::agb::kRegSoundBias, 2) == 0x0200);
    XSFLIB_CHECK((system.Cpu().Cpsr() & 0xFF) == 0x1F);
    XSFLIB_CHECK(system.Cpu().Reg(ntdlib::arm::kSp) == gsf::rip::kSystemStack);
}

void TestSongHalfword()
{
    // A plan without tables reads both bytes at kSongAddress as one halfword, and nothing after them: 0xFF at 0xC0 and
    // 0xC1 doesn't reach the number.
    CartImage image = TestImage(0x1234);
    image.Put16(kRomBase + 0xC0, 0xFFFF);
    StubPlan plan;
    plan.start = {Operation::Call(kRecord, {{5}, {1, true}})};
    const gsf::rip::Stub stub = gsf::rip::BuildStub(plan, kStubAddress);
    const agblib::agb::Rom rom = RomWithStub(image, stub);
    agblib::agb::System system(rom);

    XSFLIB_CHECK(RunStart(system, stub) == ntdlib::arm::StopReason::kStopAddress);
    XSFLIB_CHECK(system.Load(kRecorded + 4, 4) == 1 + 0x1234);
}

void TestInterruptsAndLoop()
{
    const gsf::rip::Stub stub = gsf::rip::BuildStub(TestPlan(), kStubAddress);
    const agblib::agb::Rom rom = RomWithStub(TestImage(), stub);
    agblib::agb::System system(rom);
    ntdlib::arm::Cpu& cpu = system.Cpu();
    RunStart(system, stub);

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
    const agblib::agb::Rom rom = TestImage().ToRom();
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
    XSFLIB_CHECK(run.Gba().Load(kEwramBase + 0x204, 4) == 40);
    XSFLIB_CHECK(run.Gba().Load(kEwramBase + 0x310, 4) == 0x00040302);
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
    const agblib::agb::Rom rom = TestImage().ToRom();

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

void TestGameHandler()
{
    // The game's handler in place of the stub's, with timer 1's interrupt and no vertical blank's.
    StubPlan plan = TestPlan();
    plan.interrupts = gsf::rip::kIrqTimer1;
    plan.handlers = {{gsf::rip::kIrqTimer1, {Operation::Call(kCountTimer)}, 2.5}};
    plan.game_handler = kCountTimer;
    const gsf::rip::Stub stub = gsf::rip::BuildStub(plan, kStubAddress);
    const agblib::agb::Rom rom = RomWithStub(TestImage(), stub);
    agblib::agb::System system(rom);
    ntdlib::arm::Cpu& cpu = system.Cpu();

    XSFLIB_CHECK(RunStart(system, stub) == ntdlib::arm::StopReason::kStopAddress);

    // The start runs as with the stub's handler. The game's handler is in place, with timer 1's interrupt on in IE and
    // nothing in DISPSTAT.
    XSFLIB_CHECK(system.Load(kEwramBase + 0x204, 4) == 40);
    XSFLIB_CHECK(system.Load(kRecorded, 4) == 5 && system.Load(kRecorded + 4, 4) == 1 + kSong);
    XSFLIB_CHECK(system.Load(gsf::rip::kIrqHandler, 4) == kCountTimer);
    XSFLIB_CHECK(system.Load(0x04000004, 2) == 0);
    XSFLIB_CHECK(system.Load(0x04000200, 2) == gsf::rip::kIrqTimer1);
    XSFLIB_CHECK(system.Load(0x04000208, 2) == 1);

    // The stub writes no handler. It's shorter than the same plan's stub with the stub's handler.
    StubPlan with_handler = plan;
    with_handler.game_handler = 0;
    XSFLIB_CHECK(stub.code.size() < gsf::rip::BuildStub(with_handler, kStubAddress).code.size());

    // The main loop waits with Halt for any interrupt, then runs the frame's operations.
    std::vector<uint32_t> calls;
    system.on_swi = [&](uint32_t number)
    {
        calls.push_back(number);
        if (calls.size() == 2)
        {
            cpu.Stop();
        }

        return true;
    };
    cpu.SetStopAddress(0xFFFFFFFF);

    XSFLIB_CHECK(cpu.Run(1'000'000) == ntdlib::arm::StopReason::kStopped);
    XSFLIB_CHECK(calls == (std::vector<uint32_t>{2, 2}));
    XSFLIB_CHECK(system.Load(kFrames, 4) == 1);

    // The model's runs still call each handler's operations as often as its interrupt comes.
    gsf::rip::ModelRun run(rom, plan, kSong, nullptr);
    run.Frame();
    run.Frame();

    XSFLIB_CHECK(run.Gba().Load(kTimers, 4) == 5);
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
    TestSongHalfword();
    TestInterruptsAndLoop();
    TestModelRun();
    TestStandIn();
    TestGameHandler();
    TestBranchWord();

    return xsflib::testing::Summary("stub");
}
