// SPDX-License-Identifier: MIT

// Nintendo R&D1's driver, as WarioWare, Inc.: Mega Microgame$! and Rhythm Tengoku play it. agblib finds the routines
// from the code: the init, the routine that sets the echo, the play routine, the per-frame routine and the handler of
// DMA 2's interrupt.
//
// The game calls the init, then sets the echo with four settings, and starts a song with the play routine and the
// song's entry in the song table. The mixer keeps the bytes for the FIFOs in a ring. Each time FIFO B asks for bytes,
// DMA 2 moves 16 of them, and the handler of its interrupt moves the ring's read place on. The per-frame routine runs
// once a frame and fills the ring up to a lead past the read place.
//
// How far a voice reads into its sample depends on the order of interrupts. The model runs DMA 2's handler a whole
// number of times before each frame, and the console spreads the transfers over the frame. The per-frame routine can
// then mix 16 samples more or fewer in a frame, a voice can stop a frame later, and a voice that another note takes
// over can play on. The library therefore keeps the whole of each sample that a run read part of. The handler of DMA
// 2's interrupt can also take paths on the console that the model never runs, such as Rhythm Tengoku's code at
// 0x0804917A to 0x08049240, just after the handler. The library therefore keeps the driver's code whole, from its
// lowest routine that the stub calls to the last byte that a run read within kCodeTail bytes after its highest.

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "agblib/agb/gba.h"
#include "agblib/rd1/driver.h"
#include "agblib/rd1/game_driver.h"
#include "agblib/rd1/instruments.h"
#include "agblib/sequence_export/rd1/convert.h"

#include "rip/driver.h"

namespace gsf::rip
{
namespace
{

namespace rd1 = agblib::rd1;

// The timer that plays the FIFOs counts this many cycles over the mixer's rate between two samples.
constexpr uint32_t kTimerCycles = 0xFFFED9;

// The bytes of a sample header that the driver reads, and the place of the address of the points in it.
constexpr uint32_t kSampleHeaderBytes = 24;
constexpr uint32_t kSamplePointsAddress = 20;

// A run of bytes of the cartridge.
struct Span
{
    uint32_t address = 0;
    uint32_t size = 0;
};

// The points of each sample that an instrument of a bank plays: up to the longer of its length and its loop's end, one
// byte a point.
std::vector<Span> SamplePoints(const agblib::agb::Rom& rom, const rd1::DriverInfo& info)
{
    std::set<uint32_t> headers;
    const auto add = [&](const rd1::InstrumentRecord& r)
    {
        if ((r.kind == rd1::InstrumentKind::kSample || r.kind == rd1::InstrumentKind::kFixed) &&
            rom.Contains(r.sample, kSampleHeaderBytes))
        {
            headers.insert(r.sample);
        }
    };

    for (int bank = 0; bank < info.bank_count; bank++)
    {
        for (int program = 0; program < 128; program++)
        {
            const uint32_t address = rd1::ProgramInstrument(rom, info, bank, program);
            if (!address || !rom.Contains(address))
            {
                continue;
            }

            const rd1::InstrumentRecord record = rd1::ReadInstrument(rom, address);
            if (record.kind != rd1::InstrumentKind::kDrums && record.kind != rd1::InstrumentKind::kKeySplit)
            {
                add(record);
                continue;
            }

            for (int key = 0; key < 128; key++)
            {
                add(rd1::PlayedInstrument(rom, record, key));
            }
        }
    }

    std::vector<Span> points;
    for (const uint32_t header : headers)
    {
        const uint32_t count = std::min(std::max(rom.U32(header), rom.U32(header + 16)), rd1::kMaxSamplePoints);
        const uint32_t address = rom.U32(header + kSamplePointsAddress);
        if (count && rom.Contains(address, count))
        {
            points.push_back({address, count});
        }
    }

    return points;
}

// How far past the start of the highest routine that the stub calls the driver's code can go: the init and its
// literals.
constexpr uint32_t kCodeTail = 0x1000;

class Rd1Driver : public Driver
{
public:
    Rd1Driver(const agblib::agb::Rom& rom, rd1::DriverInfo info, rd1::GameRoutines routines)
        : rom_(rom), info_(std::move(info)), routines_(std::move(routines)), samples_(SamplePoints(rom_, info_))
    {
    }

    std::string Name() const override
    {
        return "Nintendo R&D1's driver";
    }

    const std::vector<std::string>& Log() const override
    {
        return info_.log;
    }

    int SongCount() const override
    {
        return info_.song_count;
    }

    bool HasSong(int song) const override
    {
        rd1::SongRecord record;
        return rd1::ReadSongRecord(rom_, info_, song, record);
    }

    StubPlan Plan() const override
    {
        const rd1::GameRoutines& r = routines_;
        std::vector<Value> echo;
        for (const uint32_t setting : r.echo_settings)
        {
            echo.push_back({setting});
        }

        // The ring loses a byte a sample, and DMA 2 moves 16 bytes a transfer.
        const uint32_t period = kTimerCycles / uint32_t(info_.mix_rate);
        const double transfers = double(agblib::agb::kFrameCycles) / double(period * rd1::kTransferBytes);

        StubPlan plan;
        plan.start = {
            Operation::Call(r.init | 1),
            Operation::Call(r.echo | 1, echo),
            Operation::Call(r.play | 1, {{0, true}}),
        };
        plan.interrupts = kIrqVblank | kIrqDma2;
        plan.handlers = {{kIrqDma2, {Operation::Call(r.transfer | 1)}, transfers}};
        plan.frame = {Operation::Call(r.frame | 1)};
        return plan;
    }

    void CompleteReads(std::vector<uint8_t>& reads) const override
    {
        const rd1::GameRoutines& r = routines_;
        MarkCodeSpan(reads, {r.init, r.echo, r.play, r.start, r.frame, r.transfer}, kCodeTail);
        for (const Span& span : samples_)
        {
            const size_t first = span.address - agblib::agb::kRomBase;
            const size_t last = first + span.size;
            if (first >= reads.size())
            {
                continue;
            }

            const auto begin = reads.begin() + std::ptrdiff_t(first);
            const auto end = reads.begin() + std::ptrdiff_t(std::min(last, reads.size()));
            if (std::none_of(begin, end, [](uint8_t mark) { return mark != 0; }))
            {
                continue;
            }

            if (reads.size() < last)
            {
                reads.resize(last);
            }

            std::fill(reads.begin() + std::ptrdiff_t(first), reads.begin() + std::ptrdiff_t(last), uint8_t{1});
        }
    }

    std::optional<xsflib::rip::Timing> SongTiming(int song) const override
    {
        const agblib::sequence_export::agb::ConvertSettings settings;
        return TimingOfReport(agblib::sequence_export::rd1::InspectSong(rom_, info_, song, settings));
    }

private:
    const agblib::agb::Rom& rom_;
    rd1::DriverInfo info_;
    rd1::GameRoutines routines_;
    std::vector<Span> samples_;
};

} // namespace

std::unique_ptr<Driver> FindRd1(const agblib::agb::Rom& rom, std::string& error)
{
    rd1::DriverInfo info;
    if (!rd1::DetectDriver(rom, rd1::DriverOverrides{"gsfrip"}, info, error))
    {
        return nullptr;
    }

    std::optional<rd1::GameRoutines> routines = rd1::FindGameRoutines(rom, info);
    if (!routines)
    {
        error =
            "Nintendo R&D1's driver: the routines that start and run a song aren't laid out as in the known versions";
        return nullptr;
    }

    return std::make_unique<Rd1Driver>(rom, std::move(info), std::move(*routines));
}

} // namespace gsf::rip
