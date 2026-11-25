// SPDX-License-Identifier: MIT

// Engine Software's driver, a player of modules in the manner of FastTracker 2 with a software mixer. The Santa Clause
// 3: The Escape Clause plays its music on it. agblib finds the driver's build, its routines and the bank of songs, and
// gives the game's set-up and the game's start of each song as steps of the game's code.
//
// The game reaches the driver through its sound manager, a C++ object that the game's code allocates on its heap. The
// stub places the object and its block of fields in EWRAM, where agblib's GameDriver places them, and makes the
// manager's set-up without the allocation: the music and effect volumes, then the driver's reset, init, bank load and
// effect set. It then starts the song as the game does: the manager's start, which sets the music volume to 0 and
// ramps it up over the frames that follow, or for the jingle the music volume, then the manager's jingle, which plays
// the song once. The game's interrupts drive the sound: the vertical count at line 27 runs the manager's routine that
// steps the ramps and runs the driver's frame routine, which mixes the frame's samples, and the vertical blank runs the
// manager's routine that swaps the buffers and restarts the sound DMA. The stub turns on the same two interrupts, with
// the same line in DISPSTAT, and makes the same calls.
//
// The runs of the model run the same calls each frame: the vertical count's routine, then the vertical blank's. The DMA
// reads the mixer's buffers in IWRAM, never the cartridge. The library keeps the driver's code, its tables of the
// vibrato's wave and of the pitches, and the whole of each instrument that a run read part of.

#include "rip/drivers/engine_software.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "agblib/agb/rom.h"
#include "agblib/agb/system.h"
#include "agblib/engine_software/driver.h"
#include "agblib/sequence_export/agb/settings.h"
#include "agblib/sequence_export/engine_software/convert.h"

#include "rip/driver.h"

namespace gsf::rip
{
namespace
{

namespace engine_software = agblib::engine_software;

using engine_software::GameStep;

// The most arguments that a step of the game passes: the stub's calls take r0 to r3.
constexpr size_t kMaxArguments = 4;

// The songs of the bank, by number. Every one is music.
class EngineSoftwareDriver : public SongListDriver<engine_software::DriverInfo>
{
public:
    EngineSoftwareDriver(const agblib::agb::Rom& rom, engine_software::DriverInfo info)
        : SongListDriver(std::move(info)), rom_(rom), instruments_(InstrumentSpans())
    {
        LogWarnings();
    }

    std::string Name() const override
    {
        return "Engine Software's driver";
    }

    StubPlan Plan() const override
    {
        const engine_software::Build& b = info_.build;
        std::vector<std::vector<GameStep>> starts;
        for (int song = 0; song < SongCount(); song++)
        {
            starts.push_back(engine_software::StartSteps(info_, song));
        }

        StubPlan plan;
        plan.start = {Operation::Store(agblib::agb::kRegWaitCnt, {b.waitcnt}, 2)};
        for (Operation& operation : StepOperations(engine_software::SetUpSteps(info_)))
        {
            plan.start.push_back(std::move(operation));
        }

        for (Operation& operation : SongStartOperations(starts))
        {
            plan.start.push_back(std::move(operation));
        }

        // DISPSTAT's line and interrupt bits go in last, just before the stub turns the interrupts on. The set-up and
        // the start run into the next frame. Stored before them, the bits let IF latch interrupts that the handler then
        // runs at once, and the buffers swapped twice in one frame.
        plan.start.push_back(Operation::Store(agblib::agb::kRegDispStat, {b.dispstat}, 2));

        // The vertical count comes at line 27 of each frame, before the vertical blank.
        plan.interrupts = kIrqVblank | kIrqVcount;
        plan.handlers = {
            {kIrqVcount, {Operation::Call(b.manager.vcount | 1, {{b.manager.object}})}, 1.0},
            {kIrqVblank, {Operation::Call(b.manager.vblank | 1, {{b.manager.object}})}, 1.0},
        };
        return plan;
    }

    void CompleteReads(std::vector<uint8_t>& reads) const override
    {
        const engine_software::Build& b = info_.build;
        MarkSpan(reads, {b.routines.code_start, b.routines.code_end - b.routines.code_start});
        MarkSpan(reads, {b.sine, 2 * engine_software::kSinePoints});
        MarkSpan(reads, {b.pitch, 2 * engine_software::kPitchHalfwords});
        MarkSpan(reads, {b.pitch_words, 4 * engine_software::kPitchWords});
        KeepWholeSpans(reads, instruments_);
    }

    std::optional<xsflib::rip::Timing> SongTiming(int song) const override
    {
        const agblib::sequence_export::agb::ConvertSettings settings;
        return TimingOfReport(agblib::sequence_export::engine_software::InspectSong(rom_, info_, song, settings));
    }

private:
    // Each instrument's record with its samples.
    std::vector<RomSpan> InstrumentSpans() const
    {
        std::vector<RomSpan> spans;
        for (const uint32_t record : info_.bank.instruments)
        {
            spans.push_back({record, engine_software::InstrumentEnd(rom_, record) - record});
        }

        return spans;
    }

    const agblib::agb::Rom& rom_;
    std::vector<RomSpan> instruments_;
};

} // namespace

std::vector<Operation> StepOperations(std::span<const GameStep> steps)
{
    std::vector<Operation> operations;
    for (const GameStep& step : steps)
    {
        if (step.size == 0)
        {
            operations.push_back(Operation::Call(step.address, Constants(step.values)));
        }
        else
        {
            operations.push_back(Operation::Store(step.address, {step.values.at(0)}, step.size));
        }
    }

    return operations;
}

std::vector<Operation> SongStartOperations(const std::vector<std::vector<GameStep>>& starts)
{
    size_t places = 0;
    for (const std::vector<GameStep>& start : starts)
    {
        places = std::max(places, start.size());
    }

    std::vector<Operation> operations;
    for (size_t place = 0; place < places; place++)
    {
        std::vector<uint32_t> routines(starts.size(), 0);
        std::vector<std::vector<uint32_t>> arguments(kMaxArguments, std::vector<uint32_t>(starts.size(), 0));
        size_t argument_count = 0;
        for (size_t song = 0; song < starts.size(); song++)
        {
            if (place >= starts[song].size())
            {
                continue;
            }

            const GameStep& step = starts[song][place];
            if (step.size != 0 || step.values.size() > kMaxArguments)
            {
                throw std::logic_error("a song's start is calls of up to four arguments");
            }

            routines[song] = step.address;
            for (size_t a = 0; a < step.values.size(); a++)
            {
                arguments[a][song] = step.values[a];
            }

            argument_count = std::max(argument_count, step.values.size());
        }

        std::vector<Value> values;
        for (size_t a = 0; a < argument_count; a++)
        {
            values.push_back(SongTable(std::move(arguments[a])));
        }

        operations.push_back(Operation::CallFromTable(std::move(routines), std::move(values)));
    }

    return operations;
}

std::unique_ptr<Driver> FindEngineSoftware(const agblib::agb::Rom& rom, std::string& error)
{
    engine_software::DriverInfo info;
    if (!engine_software::DetectDriver(rom, info, error))
    {
        return nullptr;
    }

    return std::make_unique<EngineSoftwareDriver>(rom, std::move(info));
}

} // namespace gsf::rip
