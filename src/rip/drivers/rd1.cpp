// SPDX-License-Identifier: MIT

// Nintendo R&D1's driver, as WarioWare, Inc.: Mega Microgame$! and Rhythm Tengoku play it. agblib finds the routines in
// the code: the init, the routine that sets the echo, the play routine, the start of a song on a player that the play
// routine calls, the per-frame routine and the handler of the DMA 2 interrupt.
//
// The game calls the init, then sets the echo with four settings. It starts a song with the play routine and the entry
// of the song in the song table. The mixer keeps the bytes for the FIFOs in a ring. Each time FIFO B requests bytes,
// DMA 2 moves 16 of them, and the handler of its interrupt moves the read place of the ring on. The per-frame routine
// runs once a frame. It fills the ring up to a lead past the read place.
//
// How far a voice reads into its sample depends on the order of interrupts. The model runs the DMA 2 handler a whole
// number of times before each frame. The console spreads the transfers over the frame. The per-frame routine can then
// mix 16 samples more or fewer in a frame, and a voice can stop a frame later. A voice that another note takes over can
// play on, and read fields of an instrument record or a sample header that the runs didn't read. The library therefore
// keeps the whole of each sample, instrument record and sample header that a run read part of. On the console, the
// handler of the DMA 2 interrupt can also take paths that the model never runs, such as the code of Rhythm Tengoku at
// 0x0804917A to 0x08049240, just after the handler. The library therefore keeps the code of the driver whole: from the
// lowest of the routines that agblib finds to the last byte that a run read within kCodeTail bytes after the highest.

#include "rip/drivers/rd1.h"

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

// Between two samples, the timer that plays the FIFOs counts this many cycles over the mixer rate.
constexpr uint32_t kTimerCycles = 0xFFFED9;

// How far past the start of the highest routine of the driver its code can go: the init and its literals.
constexpr uint32_t kCodeTail = 0x1000;

class Rd1Driver : public Driver
{
public:
    Rd1Driver(const agblib::agb::Rom& rom, rd1::DriverInfo info, rd1::GameRoutines routines)
        : rom_(rom), info_(std::move(info)), routines_(std::move(routines)), whole_(Rd1WholeSpans(rom_, info_))
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

        // The ring loses a byte a sample, and DMA 2 moves 16 bytes a transfer.
        const uint32_t period = kTimerCycles / uint32_t(info_.mix_rate);
        const double transfers = double(agblib::agb::kFrameCycles) / double(period * rd1::kTransferBytes);

        StubPlan plan;
        plan.start = {
            Operation::Call(r.init | 1),
            Operation::Call(r.echo | 1, Constants(r.echo_settings)),
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
        KeepWholeSpans(reads, whole_);
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
    std::vector<RomSpan> whole_;
};

} // namespace

std::vector<RomSpan> Rd1WholeSpans(const agblib::agb::Rom& rom, const agblib::rd1::DriverInfo& info)
{
    std::set<std::pair<uint32_t, uint32_t>> records;
    std::set<uint32_t> headers;
    const auto add = [&](const rd1::InstrumentRecord& r)
    {
        if (r.kind == rd1::InstrumentKind::kNone)
        {
            return;
        }

        records.insert({r.address, rd1::RecordBytes(r.kind)});
        if ((r.kind == rd1::InstrumentKind::kSample || r.kind == rd1::InstrumentKind::kFixed) &&
            rom.Contains(r.sample, rd1::kSampleHeaderBytes))
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
            add(record);
            if (record.kind != rd1::InstrumentKind::kDrums && record.kind != rd1::InstrumentKind::kKeySplit)
            {
                continue;
            }

            for (int key = 0; key < 128; key++)
            {
                add(rd1::PlayedInstrument(rom, record, key));
            }
        }
    }

    std::vector<RomSpan> spans = SpansOf(records);
    for (const uint32_t header : headers)
    {
        spans.push_back({header, rd1::kSampleHeaderBytes});
        const rd1::SamplePoints points = rd1::PointsOfSample(rom, header);
        if (points.count && rom.Contains(points.address, points.count))
        {
            spans.push_back({points.address, points.count});
        }
    }

    return spans;
}

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
