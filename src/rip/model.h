// SPDX-License-Identifier: MIT

// The plan of a stub, run on the agblib model of the GBA (agblib::agb::System): the calls, stores and copies of the
// stub, in the same order. The model runs no interrupts. Each frame runs the handlers in the order of the plan: a
// handler of the vertical blank once, and each other handler as often as its interrupt comes in a frame. The operations
// of the main loop follow. A ripper trims a set to the bytes of the cartridge that these runs read.
//
// The model runs no timed DMA either: a DMA at the blanks or for the sound FIFOs reads nothing in a run. A run records
// each such DMA that the code starts with its source in the cartridge. A rip then checks the bytes that the DMA reads.

#pragma once

#include <array>
#include <compare>
#include <cstdint>
#include <functional>
#include <set>
#include <vector>

#include "agblib/agb/rom.h"
#include "agblib/agb/system.h"

#include "rip/stub.h"

namespace gsf::rip
{

// A DMA that transfers at the vertical or horizontal blank, or when a sound FIFO requests data. The record contains the
// values that the code started it with: the channel, the source, the count and the control (DMAxCNT_H).
struct TimedDma
{
    auto operator<=>(const TimedDma&) const = default;

    int channel = 0;
    uint32_t source = 0;
    uint32_t count = 0;
    uint16_t control = 0;
};

// The bytes of the cartridge that `dma` reads in its first transfer: the offset of the first byte in the cartridge, and
// the count. The DMA reads them whatever the order of the interrupts. A transfer of a sound FIFO is 16 bytes. The
// source of `dma` is in the cartridge, where the source always counts up.
struct DmaBytes
{
    uint32_t offset = 0;
    uint32_t size = 0;
};
DmaBytes FirstTransfer(const TimedDma& dma);

class ModelRun
{
public:
    // Loads `rom`, marks the cartridge's reads in `reads` if it isn't null, and runs the plan's start with `song` as
    // the song's number. Throws std::runtime_error when a routine fails.
    ModelRun(const agblib::agb::Rom& rom, const StubPlan& plan, uint32_t song, std::vector<uint8_t>* reads);
    ModelRun(const ModelRun&) = delete;
    ModelRun& operator=(const ModelRun&) = delete;

    // Runs a frame. Throws std::runtime_error when a routine fails.
    void Frame();

    // The frames run so far.
    uint64_t Frames() const;

    agblib::agb::System& Gba();

    // The timed DMAs that the run started with a source in the cartridge, and the count of their starts.
    const std::set<TimedDma>& TimedDmas() const;
    uint64_t TimedDmaStarts() const;

    // Called before each call of a routine, with the address of the routine. A driver can use it to stand in for a
    // routine that reads what the model doesn't have, such as the position of a DMA. It returns true when it made the
    // call in place of the routine.
    std::function<bool(agblib::agb::System& gba, uint32_t routine)> on_call;

private:
    void Run(const std::vector<Operation>& operations);
    uint32_t Resolve(const Value& value) const;

    // Records a timed DMA that a write to the I/O registers at `address` started.
    void WatchDma(uint32_t address, int size);

    StubPlan plan_;
    uint32_t song_;
    agblib::agb::System system_;
    std::vector<double> owed_; // for each handler, the runs that the frames so far still owe it
    uint64_t frames_ = 0;
    std::set<TimedDma> timed_dmas_;
    uint64_t timed_dma_starts_ = 0;
    std::array<bool, 4> dma_repeating_ = {}; // for each DMA channel, whether a timed DMA with repeat runs
};

} // namespace gsf::rip
