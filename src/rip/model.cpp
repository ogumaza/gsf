// SPDX-License-Identifier: MIT

#include "rip/model.h"

#include <cstddef>
#include <cstdint>
#include <set>
#include <vector>

namespace gsf::rip
{
namespace
{

using agblib::agb::kDmaControl;
using agblib::agb::kDmaCount;
using agblib::agb::kDmaEnable;
using agblib::agb::kDmaRepeat;
using agblib::agb::kDmaStride;
using agblib::agb::kDmaTiming;
using agblib::agb::kDmaWords;

// DMA 0 can't read the cartridge. The special timing of DMAs 1 and 2 is the requests of the sound FIFOs.
constexpr int kFirstRomDma = 1;
constexpr int kDmaChannels = 4;
constexpr int kSoundDmaLast = 2;
constexpr uint32_t kFifoTransfer = 16; // four words for each request of a sound FIFO
constexpr int kDma3 = 3;
constexpr uint32_t kMaxCount = 0x4000; // the counts of DMAs 0 to 2 have 14 bits, and 0 transfers this many
constexpr uint32_t kDma3MaxCount = 0x10000;

// The ROM of the cartridge and its mirrors, to 0x0DFFFFFF. A source above them reads the save memory.
constexpr uint32_t kRomRegionEnd = 0x0E000000;

uint32_t DmaRegister(int channel, uint32_t offset)
{
    return agblib::agb::kRegDma0Source + kDmaStride * uint32_t(channel) + offset;
}

} // namespace

DmaBytes FirstTransfer(const TimedDma& dma)
{
    // The DMA of a sound FIFO moves words whatever the control gives, and ignores the count (GBATEK, "Sound DMA"). A
    // source in the cartridge counts up by the unit whatever source step the control gives. mGBA's DMA does the same.
    // The bytes therefore run up from the source.
    const bool special = (dma.control & kDmaTiming) == kDmaTiming;
    const bool fifo = special && dma.channel >= kFirstRomDma && dma.channel <= kSoundDmaLast;
    const uint32_t unit = fifo || dma.control & kDmaWords ? 4 : 2;
    const uint32_t most = dma.channel == kDma3 ? kDma3MaxCount : kMaxCount;
    const uint32_t count = dma.count % most == 0 ? most : dma.count % most;
    const uint32_t size = fifo ? kFifoTransfer : count * unit;
    const uint32_t offset = (dma.source & ~(unit - 1)) & (agblib::agb::kMaxRomSize - 1);
    return {offset, size};
}

ModelRun::ModelRun(const agblib::agb::Rom& rom, const StubPlan& plan, uint32_t song, std::vector<uint8_t>* reads)
    : plan_(plan), song_(song), system_(rom), owed_(plan.handlers.size())
{
    system_.MarkRomReads(reads);
    system_.on_io_write = [this](uint32_t address, int size, uint32_t)
    {
        WatchDma(address, size);
    };
    Run(plan_.start);
}

void ModelRun::Frame()
{
    // The handlers of the vertical blank run once a frame. The others run as often as their interrupts come: the whole
    // part of the runs that the frames so far owe them.
    for (size_t h = 0; h < plan_.handlers.size(); h++)
    {
        const InterruptHandler& handler = plan_.handlers[h];
        if (handler.interrupt == kIrqVblank)
        {
            Run(handler.operations);
            continue;
        }

        owed_[h] += handler.per_frame;
        for (; owed_[h] >= 1.0; owed_[h] -= 1.0)
        {
            Run(handler.operations);
        }
    }

    Run(plan_.frame);
    frames_++;
}

uint64_t ModelRun::Frames() const
{
    return frames_;
}

agblib::agb::System& ModelRun::Gba()
{
    return system_;
}

const std::set<TimedDma>& ModelRun::TimedDmas() const
{
    return timed_dmas_;
}

uint64_t ModelRun::TimedDmaStarts() const
{
    return timed_dma_starts_;
}

void ModelRun::WatchDma(uint32_t address, int size)
{
    for (int channel = kFirstRomDma; channel < kDmaChannels; channel++)
    {
        const uint32_t control_register = DmaRegister(channel, kDmaControl);
        if (address >= control_register + 2 || control_register >= address + uint32_t(size))
        {
            continue;
        }

        // A timed DMA starts when a write sets the enable bit of a channel that doesn't run, with a timing other than
        // "at once". The console reads the source and the count only then: a write to the control of a DMA with repeat
        // that runs starts nothing. A DMA without repeat ends after one transfer. The model doesn't see that end. Each
        // write that sets the enable bit of such a DMA therefore counts as a start. A DMA "at once" isn't timed: the
        // model runs it after this call, and its enable bit is still set here.
        const auto control = uint16_t(system_.Load(control_register, 2));
        const bool runs = (control & kDmaEnable) && (control & kDmaTiming) != 0;
        const bool starts = runs && !dma_repeating_[size_t(channel)];
        dma_repeating_[size_t(channel)] = runs && (control & kDmaRepeat);
        const uint32_t source = system_.Load(DmaRegister(channel, 0), 4) & 0x0FFFFFFF;
        if (!starts || source < agblib::agb::kRomBase || source >= kRomRegionEnd)
        {
            continue;
        }

        timed_dma_starts_++;
        timed_dmas_.insert({channel, source, system_.Load(DmaRegister(channel, kDmaCount), 2), control});
    }
}

void ModelRun::Run(const std::vector<Operation>& operations)
{
    for (const Operation& operation : operations)
    {
        switch (operation.kind)
        {
        case Operation::Kind::kCall:
            {
                // A call from a table skips the songs whose entry is 0.
                const bool from_table = !operation.routines.empty();
                uint32_t routine = operation.address;
                if (from_table)
                {
                    routine = song_ < operation.routines.size() ? operation.routines[song_] : 0;
                }

                if ((from_table && routine == 0) || (on_call && on_call(system_, routine)))
                {
                    break;
                }

                std::vector<uint32_t> args;
                for (const Value& value : operation.values)
                {
                    args.push_back(Resolve(value));
                }

                system_.Call(routine, args);
            }
            break;

        case Operation::Kind::kStore:
            system_.Store(operation.address, int(operation.size), Resolve(operation.values.at(0)));
            break;

        case Operation::Kind::kCopy:
            for (uint32_t i = 0; i < operation.size; i++)
            {
                system_.Store(operation.destination + i, 1, system_.Load(operation.address + i, 1));
            }
            break;
        }
    }
}

uint32_t ModelRun::Resolve(const Value& value) const
{
    if (!value.table.empty())
    {
        return song_ < value.table.size() ? value.table[song_] : 0;
    }

    return value.constant + (value.song ? song_ : 0);
}

} // namespace gsf::rip
