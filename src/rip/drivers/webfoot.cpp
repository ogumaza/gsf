// SPDX-License-Identifier: MIT

// Webfoot Technologies' driver. The Legacy of Goku games, Buu's Fury and GT Transformation play their music on it.
// agblib finds the song tables, the copy of the driver that plays the songs of each table, and the routines of that
// copy in the code of the game. A cartridge can contain several programs, each with a copy of the driver and a song
// table. Such a game has a set for each program, through the table of games with parts (driver.cpp).
//
// The game calls the init of the driver, the routine of the music volume with the volume of its options, and the play
// routine with the header of a song. The crt0 of the later build first copies an image of IWRAM that contains the
// interrupt handler and the mixer of the driver. The stub makes the same copy and calls, with the headers in a table by
// song. It first sets WAITCNT to the value that the game has when its music plays.
//
// The interrupt handler of the driver runs for the DMA 2 interrupt or the timer 1 interrupt, every 16 or 64 samples. It
// returns through a register that the handler of the game sets, and reads the I/O base from r3. When the lead of the
// mixer falls to its mark, it turns the interrupts on and jumps to the mix routine. The next interrupt then comes
// inside the mix. The handler of the stub can't run it. The stub therefore puts the handler of the game at 0x03007FFC
// (StubPlan::game_handler). agblib finds that handler (GameRoutines::master_handler). The init of the driver stores its
// handler in a table of the game, and the handler of the game reads that table. Like the GameDriver of agblib, the runs
// of the model call the handler of the driver through CallHandler().
//
// The model counts no cycles. Where a mix falls elsewhere among the interrupts, the console can read more of a sample
// than the runs of the model did. The library therefore keeps the whole of each sample that a run read part of, with
// its entry in the sample table. It also keeps the code of the driver in the cartridge around the routines that the
// stub calls, and the tables of steps, slides and vibrato. The runs of the model don't call the handler of the game.
// The library keeps that handler whole where it runs from the cartridge. The DMAs of the FIFOs read the rings of the
// mixer in the work memory, never the cartridge.

#include "rip/drivers/webfoot.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "agblib/agb/gba.h"
#include "agblib/agb/rom.h"
#include "agblib/agb/system.h"
#include "agblib/sequence_export/webfoot/convert.h"
#include "agblib/webfoot/driver.h"
#include "agblib/webfoot/game_driver.h"

#include "rip/driver.h"

namespace gsf::rip
{
namespace
{

namespace webfoot = agblib::webfoot;

using agblib::agb::Hex;
using agblib::agb::kIoBase;
using agblib::agb::kIwramBase;
using agblib::agb::kRegWaitCnt;
using agblib::agb::kRomBase;

// The start of the crt0 in a program, after the cartridge header, and the bytes of the crt0 that Crt0WaitCnt() reads.
constexpr uint32_t kCrt0Start = 0xC0;
constexpr uint32_t kCrt0WaitCntReach = 0x100;

// The bytes of the handler of the game with its literal pool: 0x08000138 to 0x08000274 in The Legacy of Goku.
constexpr uint32_t kGameHandlerSize = 0x13C;

// The interrupts that the init of the driver can turn on: the DMA 2 interrupt (the 2002 build, and the games of the
// later build with one program) and the timer 1 interrupt (the cartridge of the later build with several programs).
constexpr uint16_t kDriverIrqs = kIrqDma2 | kIrqTimer1;

// WAITCNT as a game has it when its music plays, by the game code, the version and the song table of the program among
// the tables of the cartridge. The code of the games sets it from a record of their data. The values therefore came
// from runs of the games in mGBA 0.11. The menu of the cartridge with two games keeps the 0x4014 of its crt0. A game
// that the table doesn't name gets the value of its crt0 (Crt0WaitCnt()). With the WAITCNT of the BIOS, the mix ran
// later in the stub than in the game. In some songs, the DMAs of the FIFOs then read 16 bytes of the ring before the
// mix had written them.
struct GameWaitCnt
{
    std::string_view code;
    uint8_t version = 0;
    int part = 0;
    uint16_t waitcnt = 0;
};

constexpr GameWaitCnt kGameWaitCnts[] = {
    {"ALGE", 0, 0, 0x4314}, {"ALFE", 0, 0, 0x4314}, {"BG3E", 0, 0, 0x4314},
    {"BUFE", 0, 0, 0x4014}, {"BUFE", 0, 1, 0x4314}, {"BUFE", 0, 2, 0x4314},
};

// The WAITCNT of song table `part` of the game in `rom`, or 0 for a game that the table doesn't name.
uint16_t TableWaitCnt(const agblib::agb::Rom& rom, int part)
{
    const std::string code = rom.GameCode();
    const uint8_t version = rom.Version();
    for (const GameWaitCnt& game : kGameWaitCnts)
    {
        if (game.code == code && game.version == version && game.part == part)
        {
            return game.waitcnt;
        }
    }

    return 0;
}

// Returns the warning of the rip for `table`, whose init sets up the mixer otherwise than the model of agblib has it
// (SongTable::mixer_check). agblib words its warning for its model. That model plays the songs at the rate of
// MixerOf(). A set plays the code of the game. Only the lengths and the trim of the set depend on the model.
std::string MixerWarning(const webfoot::SongTable& table)
{
    const std::string program = "the program at " + Hex(table.program);
    const std::string effect =
        ". The set plays the game's code at the game's pitch and tempo. The lengths and the trim come from the "
        "model and may be wrong";
    if (table.mixer_check == webfoot::MixerCheck::kDiffers)
    {
        return "the init in " + program + " sets up the mixer differently from gsfrip's model of the driver" + effect;
    }

    return "can't read the mixer's settings in " + program + effect;
}

// Returns true if entry `index` of the warnings of the detection is the agblib warning of the mixer check of one of the
// song tables. MixerWarning() words that warning for a rip.
bool IsMixerWarning(const webfoot::DriverInfo& info, size_t index)
{
    for (const webfoot::SongTable& table : info.song_tables)
    {
        if (table.mixer_check != webfoot::MixerCheck::kSame && table.mixer_warning_index == index)
        {
            return true;
        }
    }

    return false;
}

class WebfootDriver : public Driver
{
public:
    WebfootDriver(const agblib::agb::Rom& rom, webfoot::DriverInfo info, int part,
                  const webfoot::GameRoutines& routines, uint16_t interrupt, uint16_t waitcnt)
        : rom_(rom), info_(std::move(info)), part_(part), routines_(routines), interrupt_(interrupt), waitcnt_(waitcnt)
    {
        const webfoot::SongTable& table = Table();
        info_.log.push_back("the song table at " + Hex(table.address) +
                            ", with the copy of the driver of the program at " + Hex(routines_.program) + ": init " +
                            Hex(routines_.init) + ", music volume " + Hex(routines_.music_volume) + " (" +
                            std::to_string(table.music_volume) + "), play " + Hex(routines_.play) +
                            ", the driver's interrupt handler " + Hex(routines_.handler) + ", the game's " +
                            Hex(routines_.master_handler));
        if (waitcnt_ == 0)
        {
            info_.log.push_back(
                "warning: no WAITCNT for the song table at " + Hex(table.address) +
                ", and the program's crt0 sets none: the stub leaves the BIOS's, and the mix can run late");
        }
        else if (TableWaitCnt(rom_, part_) == 0)
        {
            info_.log.push_back("warning: no WAITCNT for the song table at " + Hex(table.address) +
                                ": the stub sets the " + Hex(waitcnt_) +
                                " of the program's crt0, and the game may set another before its music plays");
        }

        for (const webfoot::SongTable& checked : info_.song_tables)
        {
            if (checked.mixer_check != webfoot::MixerCheck::kSame)
            {
                info_.log.push_back("warning: " + MixerWarning(checked));
            }
        }

        for (size_t i = 0; i < info_.warnings.size(); i++)
        {
            if (!IsMixerWarning(info_, i))
            {
                info_.log.push_back("warning: " + info_.warnings[i]);
            }
        }

        samples_ = DriverSamples();
    }

    std::string Name() const override
    {
        return "Webfoot Technologies' driver";
    }

    const std::vector<std::string>& Log() const override
    {
        return info_.log;
    }

    // The number of a song counts through all of the song tables of the image. agblib numbers the songs the same way.
    // The set has the songs of the table of its part.
    int SongCount() const override
    {
        return info_.SongCount();
    }

    bool HasSong(int song) const override
    {
        int index = 0;
        return info_.TableOf(song, index) == &Table();
    }

    StubPlan Plan() const override
    {
        const webfoot::SongTable& table = Table();
        std::vector<uint32_t> headers(size_t(SongCount()), 0);
        for (int song = 0; song < SongCount(); song++)
        {
            int index = 0;
            if (info_.TableOf(song, index) == &table)
            {
                headers[size_t(song)] = table.songs[size_t(index)].address;
            }
        }

        // The routines of the 2002 build take the volume and the header. Those of the later build take the music player
        // first.
        StubPlan plan;
        if (waitcnt_ != 0)
        {
            plan.start.push_back(Operation::Store(kRegWaitCnt, {waitcnt_}, 2));
        }

        if (routines_.iwram_size != 0)
        {
            plan.start.push_back(Operation::Copy(routines_.iwram_source, kIwramBase, routines_.iwram_size));
        }

        plan.start.push_back(Operation::Call(routines_.init | 1, {{routines_.init_argument}}));

        const Value volume = {uint32_t(table.music_volume)};
        if (table.build == webfoot::Build::k2002)
        {
            plan.start.push_back(Operation::Call(routines_.music_volume | 1, {volume}));
            plan.start.push_back(Operation::Call(routines_.play | 1, {SongTable(headers)}));
        }
        else
        {
            plan.start.push_back(Operation::Call(routines_.music_volume | 1, {{routines_.player}, volume}));
            plan.start.push_back(Operation::Call(routines_.play | 1, {{routines_.player}, SongTable(headers)}));
        }

        // The interrupt comes once every samples_per_interrupt samples of the FIFOs.
        const webfoot::Mixer mixer = webfoot::MixerOf(table.build);
        const double per_frame =
            double(agblib::agb::kFrameCycles) / double(mixer.timer_cycles * routines_.samples_per_interrupt);
        plan.interrupts = interrupt_;
        plan.handlers = {{interrupt_, {Operation::Call(routines_.handler)}, per_frame}};
        plan.game_handler = routines_.master_handler;
        return plan;
    }

    // The runs of the model call the handler of the driver as the handler of the game does (webfoot::CallHandler()).
    void PrepareModel(ModelRun& run) const override
    {
        run.on_call = [routines = routines_](agblib::agb::System& gba, uint32_t routine)
        {
            if (routine != routines.handler)
            {
                return false;
            }

            webfoot::CallHandler(gba, routines);
            return true;
        };
    }

    void CompleteReads(std::vector<uint8_t>& reads) const override
    {
        // The code of the driver in the cartridge: from the lowest byte that a run read within kCodeReach below the
        // lowest routine that the stub calls, to the last within kCodeReach after the highest. The routines that the
        // stub calls aren't the lowest of the driver: the note routine of the later build is 0xC2C bytes below its play
        // routine (0x0801FE20 in The Legacy of Goku II). The image of IWRAM stays whole: each run copies it.
        std::vector<uint32_t> routines = {routines_.init & ~1u, routines_.music_volume & ~1u, routines_.play & ~1u};
        if (routines_.handler >= kRomBase)
        {
            routines.push_back(routines_.handler & ~1u);
        }

        const auto [lowest, highest] = std::minmax_element(routines.begin(), routines.end());
        uint32_t first = *lowest;
        for (uint32_t at = *lowest - kCodeReach; at < *lowest; at++)
        {
            if (at - kRomBase < reads.size() && reads[at - kRomBase])
            {
                first = at;
                break;
            }
        }

        MarkCodeSpan(reads, {first, *highest}, kCodeReach);

        // The handler of the game, where it runs from the cartridge. The runs of the model don't call it.
        if (routines_.master_handler >= kRomBase)
        {
            MarkSpan(reads, {routines_.master_handler, kGameHandlerSize});
        }

        // The tables of the steps of the notes, the slides and the vibrato, whole.
        const webfoot::Tables& tables = Table().tables;
        for (const RomSpan& table : {RomSpan{tables.steps, 4 * webfoot::kNotes},
                                     RomSpan{tables.slide_up - webfoot::kFineSlideBytes, webfoot::kSlideTableBytes},
                                     RomSpan{tables.slide_down - webfoot::kFineSlideBytes, webfoot::kSlideTableBytes},
                                     RomSpan{tables.vibrato, webfoot::kVibratoBytes}})
        {
            MarkSpan(reads, table);
        }

        // The entry and the points of each sample, where a run read either.
        for (size_t i = 0; i + 1 < samples_.size(); i += 2)
        {
            if (AnyRead(reads, samples_[i]) || AnyRead(reads, samples_[i + 1]))
            {
                MarkSpan(reads, samples_[i]);
                MarkSpan(reads, samples_[i + 1]);
            }
        }
    }

    std::optional<xsflib::rip::Timing> SongTiming(int song) const override
    {
        const agblib::sequence_export::agb::ConvertSettings settings;
        return TimingOfReport(agblib::sequence_export::webfoot::InspectSong(rom_, info_, song, settings));
    }

private:
    // How far the code of the driver can reach below the lowest routine that the stub calls, and after the highest.
    static constexpr uint32_t kCodeReach = 0x1000;

    // The most entries of a sample table that the spans of the samples cover: the instruments of a row are bytes.
    static constexpr uint32_t kMaxSamples = 256;

    const webfoot::SongTable& Table() const
    {
        return info_.song_tables[size_t(part_)];
    }

    // The spans of the samples of the songs of the part, in pairs: each entry of 12 bytes, then its points. The entries
    // of a table run until one whose points aren't all in the cartridge.
    std::vector<RomSpan> DriverSamples() const
    {
        std::set<uint32_t> sample_tables;
        for (const webfoot::SongHeader& header : Table().songs)
        {
            sample_tables.insert(header.samples);
        }

        std::vector<RomSpan> spans;
        for (const uint32_t sample_table : sample_tables)
        {
            for (uint32_t i = 0; i < kMaxSamples; i++)
            {
                const uint32_t entry = sample_table + webfoot::kSampleSize * i;
                if (!rom_.Contains(entry, webfoot::kSampleSize))
                {
                    break;
                }

                const webfoot::Sample sample = webfoot::ReadSample(rom_, entry);
                if (sample.length == 0 || !rom_.Contains(sample.points, sample.length))
                {
                    break;
                }

                spans.push_back({entry, webfoot::kSampleSize});
                spans.push_back({sample.points, sample.length});
            }
        }

        return spans;
    }

    const agblib::agb::Rom& rom_;
    webfoot::DriverInfo info_;
    int part_ = 0;
    webfoot::GameRoutines routines_;
    uint16_t interrupt_ = 0;
    uint16_t waitcnt_ = 0;
    std::vector<RomSpan> samples_;
};

// The driver of song table `part` of what detection found in `rom`, or null with `error` set.
std::unique_ptr<Driver> DriverOfPart(const agblib::agb::Rom& rom, webfoot::DriverInfo info, int part,
                                     std::string& error)
{
    if (part < 0 || size_t(part) >= info.song_tables.size())
    {
        error = "Webfoot Technologies' driver: the image has " + std::to_string(info.song_tables.size()) +
                " song tables, and no table " + std::to_string(part);
        return nullptr;
    }

    const webfoot::SongTable& table = info.song_tables[size_t(part)];
    webfoot::GameRoutines routines;
    if (!webfoot::FindGameRoutines(rom, table, routines, error))
    {
        error = "Webfoot Technologies' driver: the song table at " + Hex(table.address) + ": " + error;
        return nullptr;
    }

    if (routines.master_handler == 0)
    {
        error = "Webfoot Technologies' driver: the program at " + Hex(routines.program) +
                " has no interrupt handler that gsf knows";
        return nullptr;
    }

    // The init turns on the interrupt of the driver in IE.
    uint16_t interrupt = 0;
    try
    {
        webfoot::GameDriver game(rom, routines);
        interrupt = game.InterruptsEnabled() & kDriverIrqs;
    }
    catch (const std::exception& e)
    {
        error = "Webfoot Technologies' driver: the init at " + Hex(routines.init) + " failed: " + e.what();
        return nullptr;
    }

    if (interrupt != kIrqDma2 && interrupt != kIrqTimer1)
    {
        error = "Webfoot Technologies' driver: the init at " + Hex(routines.init) +
                " turns on neither DMA 2's interrupt nor timer 1's";
        return nullptr;
    }

    const uint16_t table_waitcnt = TableWaitCnt(rom, part);
    const uint16_t waitcnt = table_waitcnt != 0 ? table_waitcnt : Crt0WaitCnt(rom, routines.program);

    return std::make_unique<WebfootDriver>(rom, std::move(info), part, routines, interrupt, waitcnt);
}

} // namespace

uint16_t Crt0WaitCnt(const agblib::agb::Rom& rom, uint32_t program)
{
    // The known values of the registers, from `mov rN, #imm` and `ldr rN, [pc, #imm]`.
    std::array<std::optional<uint32_t>, 16> regs = {};
    const uint32_t start = program + kCrt0Start;
    for (uint32_t at = start; at < start + kCrt0WaitCntReach && rom.Contains(at, 4); at += 4)
    {
        const uint32_t w = rom.U32(at);
        const uint32_t n = (w >> 16) & 15u;
        const uint32_t d = (w >> 12) & 15u;
        if ((w & 0xFFF00000) == 0xE5800000 && ((w & 0xFFF) == 0x204 || (w & 0xFFF) == 0))
        {
            // str rD, [rN, #0x204] or str rD, [rN]
            const uint32_t address = (w & 0xFFF) == 0x204 ? kIoBase : kRegWaitCnt;
            if (regs[n] == address && regs[d])
            {
                return uint16_t(*regs[d]);
            }
            continue;
        }

        if ((w & 0xFFFF0000) == 0xE3A00000)
        {
            // mov rD, #imm: 8 bits rotated right by twice the rotation.
            regs[d] = std::rotr(w & 0xFFu, int(2 * ((w >> 8) & 15u)));
        }
        else if ((w & 0xFF7F0000) == 0xE51F0000)
        {
            // ldr rD, [pc, #±imm]
            const uint32_t offset = w & 0xFFF;
            const uint32_t literal = (w & 0x00800000) ? at + 8 + offset : at + 8 - offset;
            regs[d] = rom.Contains(literal, 4) ? std::optional<uint32_t>(rom.U32(literal)) : std::nullopt;
        }
        else if ((w & 0x0C000000) == 0 || (w & 0x0C100000) == 0x04100000)
        {
            // Any other instruction of data processing, or a load, can write rD.
            regs[d].reset();
        }
    }

    return 0;
}

std::unique_ptr<Driver> FindWebfootPart(const agblib::agb::Rom& rom, int part, std::string& error)
{
    webfoot::DriverInfo info;
    if (!webfoot::DetectDriver(rom, info, error))
    {
        return nullptr;
    }

    return DriverOfPart(rom, std::move(info), part, error);
}

std::unique_ptr<Driver> FindWebfoot(const agblib::agb::Rom& rom, std::string& error)
{
    webfoot::DriverInfo info;
    if (!webfoot::DetectDriver(rom, info, error))
    {
        return nullptr;
    }

    if (info.song_tables.size() != 1)
    {
        error = "Webfoot Technologies' driver: the image has " + std::to_string(info.song_tables.size()) +
                " song tables, one for each program, and gsf has no list of this game's parts";
        return nullptr;
    }

    return DriverOfPart(rom, std::move(info), 0, error);
}

} // namespace gsf::rip
