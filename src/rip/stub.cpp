// SPDX-License-Identifier: MIT

#include "rip/stub.h"

#include <cstdint>
#include <stdexcept>
#include <utility>
#include <vector>

#include "rip/arm.h"

namespace gsf::rip
{
namespace
{

// The I/O registers that the stub writes, from the start of the I/O.
constexpr uint32_t kIoBase = 0x04000000;
constexpr int kDispStat = 0x004;
constexpr int kSoundBias = 0x088;
constexpr uint32_t kBootSoundBias = 0x0200; // what the BIOS leaves in SOUNDBIAS, by GBATEK
constexpr uint32_t kInterruptBlock = 0x200; // IE, then IF at +2 and IME at +8
constexpr int kIf = 2;
constexpr int kIme = 8;
constexpr uint32_t kVblankIrqEnable = 0x0008; // DISPSTAT's bit

// The BIOS's calls that the main loop waits with.
constexpr uint32_t kHalt = 0x02;
constexpr uint32_t kVblankIntrWait = 0x05;

// The registers of the stub's operations. A call takes its arguments in r0 to r3 and its routine in r12. The stub
// keeps nothing in a register across a call.
constexpr int kR0 = 0;
constexpr int kR1 = 1;
constexpr int kR2 = 2;
constexpr int kR3 = 3;
constexpr int kR12 = 12;

constexpr uint16_t Bit(int r)
{
    return uint16_t(1u << r);
}

// Assembles each of a plan's operations.
class OperationWriter
{
public:
    OperationWriter(ArmAssembler& a, int song_word) : a_(a), song_word_(song_word)
    {
    }

    void Write(const std::vector<Operation>& operations)
    {
        for (const Operation& operation : operations)
        {
            Write(operation);
        }
    }

    // The tables of the values that Write() loaded from one, each at its label.
    void WriteTables()
    {
        for (const auto& [label, table] : tables_)
        {
            a_.Bind(label);
            for (const uint32_t word : table)
            {
                a_.Word(word);
            }
        }
    }

private:
    void Write(const Operation& operation)
    {
        switch (operation.kind)
        {
        case Operation::Kind::kCall:
            {
                if (operation.values.size() > 4)
                {
                    throw std::logic_error("the stub passes up to four arguments");
                }

                for (size_t i = 0; i < operation.values.size(); i++)
                {
                    Load(int(i), operation.values[i]);
                }

                if (!operation.routines.empty())
                {
                    CallFromTable(operation.routines);
                    break;
                }

                a_.LoadConstant(kR12, operation.address);
                if (operation.address & 1)
                {
                    a_.CallThumb(kR12);
                }
                else
                {
                    a_.CallRegister(kR12);
                }
            }
            break;

        case Operation::Kind::kStore:
            {
                Load(kR0, operation.values.at(0));
                a_.LoadConstant(kR1, operation.address);
                if (operation.size == 1)
                {
                    a_.Strb(kR0, kR1);
                }
                else if (operation.size == 2)
                {
                    a_.Strh(kR0, kR1);
                }
                else if (operation.size == 4)
                {
                    a_.Str(kR0, kR1);
                }
                else
                {
                    throw std::logic_error("a store is of 1, 2 or 4 bytes");
                }
            }
            break;

        case Operation::Kind::kCopy:
            {
                Copy(operation);
            }
            break;
        }
    }

    // Calls the song's Thumb routine from `routines`, or skips the call for an entry of 0. The arguments are in r0 to
    // r3 already, so the table's address goes in lr, which the call sets anyway.
    void CallFromTable(const std::vector<uint32_t>& routines)
    {
        for (const uint32_t routine : routines)
        {
            if (routine != 0 && !(routine & 1))
            {
                throw std::logic_error("a table's routines are Thumb code");
            }
        }

        const int label = a_.NewLabel();
        const int skip = a_.NewLabel();
        tables_.emplace_back(label, routines);
        a_.LoadAddress(kR12, song_word_);
        a_.Ldr(kR12, kR12);
        a_.LoadAddress(kLr, label);
        a_.LdrIndexed(kR12, kLr, kR12, 2);
        a_.Sub(kR12, kR12, 0, true);
        a_.B(skip, Condition::kEq);
        a_.CallThumb(kR12);
        a_.Bind(skip);
    }

    // rd = value. A load from a table uses r12, which a call sets to its routine after its arguments.
    void Load(int rd, const Value& value)
    {
        if (!value.table.empty())
        {
            const int label = a_.NewLabel();
            tables_.emplace_back(label, value.table);
            a_.LoadAddress(kR12, song_word_);
            a_.Ldr(kR12, kR12);
            a_.LoadAddress(rd, label);
            a_.LdrIndexed(rd, rd, kR12, 2);
            return;
        }

        if (!value.song)
        {
            a_.LoadConstant(rd, value.constant);
            return;
        }

        a_.LoadAddress(rd, song_word_);
        a_.Ldr(rd, rd);
        if (value.constant != 0)
        {
            a_.Add(rd, rd, value.constant);
        }
    }

    // A loop of words where the size is a multiple of 4, or else of bytes.
    void Copy(const Operation& operation)
    {
        if (operation.size == 0)
        {
            return;
        }

        const bool words = operation.size % 4 == 0 && operation.address % 4 == 0 && operation.destination % 4 == 0;
        a_.LoadConstant(kR0, operation.address);
        a_.LoadConstant(kR1, operation.destination);
        a_.LoadConstant(kR2, words ? operation.size / 4 : operation.size);

        const int loop = a_.NewLabel();
        a_.Bind(loop);
        if (words)
        {
            a_.LdrPost(kR3, kR0, 4);
            a_.StrPost(kR3, kR1, 4);
        }
        else
        {
            a_.LdrbPost(kR3, kR0, 1);
            a_.StrbPost(kR3, kR1, 1);
        }
        a_.Sub(kR2, kR2, 1, true);
        a_.B(loop, Condition::kNe);
    }

    ArmAssembler& a_;
    int song_word_;
    std::vector<std::pair<int, std::vector<uint32_t>>> tables_;
};

// The interrupt handler that the BIOS calls in IRQ mode, with r0 to r3, r12 and lr saved. It acknowledges the
// interrupts that are on and have come, in IF and in the BIOS's flags. It then runs their operations in system mode,
// with IRQs still off, on the stack of the code that the interrupt stopped.
void WriteInterruptHandler(ArmAssembler& a, OperationWriter& writer, const StubPlan& plan)
{
    a.Mov(kR12, kIoBase);
    a.Add(kR12, kR12, kInterruptBlock);
    a.Ldr(kR0, kR12);
    a.AndShifted(kR0, kR0, kR0, 16);
    a.Strh(kR0, kR12, kIf);
    a.LoadConstant(kR1, kBiosIrqFlags);
    a.Ldrh(kR2, kR1);
    a.OrrRegister(kR2, kR2, kR0);
    a.Strh(kR2, kR1);

    a.SetControl(kModeSystem | kIrqsOff | kFiqsOff);
    a.Push(Bit(kR0) | Bit(kLr));
    for (const InterruptHandler& handler : plan.handlers)
    {
        const int next = a.NewLabel();
        a.Ldr(kR0, kSp);
        a.Tst(kR0, handler.interrupt);
        a.B(next, Condition::kEq);
        writer.Write(handler.operations);
        a.Bind(next);
    }
    a.Pop(Bit(kR0) | Bit(kLr));
    a.SetControl(kModeIrq | kIrqsOff | kFiqsOff);
    a.Bx(kLr);
}

} // namespace

Value SongTable(std::vector<uint32_t> table)
{
    Value value;
    value.table = std::move(table);
    return value;
}

Operation Operation::Call(uint32_t routine, std::vector<Value> args)
{
    Operation o;
    o.kind = Kind::kCall;
    o.address = routine;
    o.values = std::move(args);
    return o;
}

Operation Operation::CallFromTable(std::vector<uint32_t> routines, std::vector<Value> args)
{
    Operation o = Call(0, std::move(args));
    o.routines = std::move(routines);
    return o;
}

Operation Operation::Store(uint32_t address, Value value, int size)
{
    Operation o;
    o.kind = Kind::kStore;
    o.address = address;
    o.size = uint32_t(size);
    o.values = {value};
    return o;
}

Operation Operation::Copy(uint32_t source, uint32_t destination, uint32_t size)
{
    Operation o;
    o.kind = Kind::kCopy;
    o.address = source;
    o.destination = destination;
    o.size = size;
    return o;
}

uint32_t StubSize(const StubPlan& plan)
{
    // The size doesn't depend on the address: the literal pool keeps each label's address apart from the constants,
    // even where the two hold the same value.
    return uint32_t(BuildStub(plan, 0x08000000, 0).code.size());
}

Stub BuildStub(const StubPlan& plan, uint32_t address, uint32_t song)
{
    ArmAssembler a(address);
    const int song_word = a.NewLabel();
    const int interrupt_handler = a.NewLabel();
    const int loop = a.NewLabel();
    OperationWriter writer(a, song_word);

    // The stacks of the modes that the stub runs in, with the IRQs and the FIQs off.
    a.SetControl(kModeIrq | kIrqsOff | kFiqsOff);
    a.LoadConstant(kSp, kIrqStack);
    a.SetControl(kModeSupervisor | kIrqsOff | kFiqsOff);
    a.LoadConstant(kSp, kSupervisorStack);
    a.SetControl(kModeSystem | kIrqsOff | kFiqsOff);
    a.LoadConstant(kSp, kSystemStack);

    // SOUNDBIAS as the BIOS leaves it, IME off, and the handler in place.
    a.Mov(kR0, kIoBase);
    a.LoadConstant(kR1, kBootSoundBias);
    a.Strh(kR1, kR0, kSoundBias);
    a.Add(kR0, kR0, kInterruptBlock);
    a.Mov(kR1, 0);
    a.Strh(kR1, kR0, kIme);
    a.LoadAddress(kR0, interrupt_handler);
    a.LoadConstant(kR1, kIrqHandler);
    a.Str(kR0, kR1);

    // The game's set-up and the song's start.
    writer.Write(plan.start);

    // The interrupts on: the vertical blank's in DISPSTAT, each one in IE, then IME and the CPSR.
    a.Mov(kR2, kIoBase);
    if (plan.interrupts & kIrqVblank)
    {
        a.Ldrh(kR0, kR2, kDispStat);
        a.Orr(kR0, kR0, kVblankIrqEnable);
        a.Strh(kR0, kR2, kDispStat);
    }
    a.Add(kR2, kR2, kInterruptBlock);
    a.Ldrh(kR0, kR2);
    a.LoadConstant(kR1, plan.interrupts);
    a.OrrRegister(kR0, kR0, kR1);
    a.Strh(kR0, kR2);
    a.Mov(kR0, 1);
    a.Strh(kR0, kR2, kIme);
    a.SetControl(kModeSystem);

    // The main loop: a wait for the next vertical blank, or for any interrupt without one, then the frame's work.
    a.Bind(loop);
    a.Swi(plan.interrupts & kIrqVblank ? kVblankIntrWait : kHalt);
    writer.Write(plan.frame);
    a.B(loop);

    a.Bind(interrupt_handler);
    WriteInterruptHandler(a, writer, plan);

    // The literal pool, then the song's word and the tables, which can be larger than a load's reach.
    a.Pool();
    a.Bind(song_word);
    a.Word(song);
    writer.WriteTables();

    Stub stub;
    stub.address = address;
    stub.song_address = a.Address(song_word);
    stub.loop = a.Address(loop);
    stub.code = a.Finish();
    return stub;
}

uint32_t BranchWord(uint32_t from, uint32_t to)
{
    const int64_t offset = (int64_t(to) - (int64_t(from) + 8)) / 4;
    if ((to - from) % 4 != 0 || offset < -0x800000 || offset > 0x7FFFFF)
    {
        throw std::logic_error("the stub lies out of the first word's reach");
    }

    return 0xEA000000 | (uint32_t(offset) & 0xFFFFFF);
}

} // namespace gsf::rip
