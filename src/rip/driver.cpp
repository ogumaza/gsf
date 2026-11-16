// SPDX-License-Identifier: MIT

#include "rip/driver.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "agblib/agb/gba.h"

namespace gsf::rip
{

bool Driver::HasEffect(int) const
{
    return false;
}

StubPlan Driver::PlanWithEffects() const
{
    return Plan();
}

const GameTags* Driver::Tags() const
{
    return nullptr;
}

void Driver::PrepareModel(ModelRun&) const
{
}

void Driver::CompleteReads(std::vector<uint8_t>&) const
{
}

bool Driver::KeepsTimedDmaReads() const
{
    return false;
}

std::optional<long long> Driver::LastSoundMs(int) const
{
    return std::nullopt;
}

void MarkCodeSpan(std::vector<uint8_t>& reads, std::initializer_list<uint32_t> routines, uint32_t tail)
{
    const auto [lowest, highest] = std::minmax(routines);
    if (lowest < agblib::agb::kRomBase)
    {
        return;
    }

    const size_t first = (lowest & ~1u) - agblib::agb::kRomBase;
    const size_t start = (highest & ~1u) - agblib::agb::kRomBase;
    const size_t end = std::min(reads.size(), start + tail);
    size_t last = start;
    for (size_t i = start; i < end; i++)
    {
        if (reads[i])
        {
            last = i;
        }
    }

    if (last < reads.size())
    {
        std::fill(reads.begin() + std::ptrdiff_t(first), reads.begin() + std::ptrdiff_t(last + 1), uint8_t{1});
    }
}

void KeepWholeSpans(std::vector<uint8_t>& reads, std::span<const RomSpan> spans)
{
    for (const RomSpan& span : spans)
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

std::optional<xsflib::rip::Timing> TimingOfReport(const agblib::sequence_export::agb::SongReport& report)
{
    if (!report.ok)
    {
        return std::nullopt;
    }

    if (report.silent)
    {
        xsflib::rip::Timing timing;
        timing.silent = true;
        return timing;
    }

    // The report counts whole frames.
    const double frame_ms = 1000.0 / agblib::agb::kFrameRate;
    const auto frames = uint64_t(std::llround(report.seconds * agblib::agb::kFrameRate));

    return report.loop_start >= 0 ? xsflib::rip::LoopAt(frames, frame_ms, kFadeMs)
                                  : xsflib::rip::EndAt(frames, frame_ms, kTailMs);
}

namespace
{

using Find = std::unique_ptr<Driver> (*)(const agblib::agb::Rom&, std::string&);

// A set of a game with more than one: the driver of its part of the game, and the set's name.
struct PartSet
{
    Find find = nullptr;
    const char* name = "";
};

// The games with a driver for each part, by the game code in the cartridge's header. The Legend of Zelda: A Link to the
// Past & Four Swords plays A Link to the Past with Nintendo R&D2's driver, and its menus and Four Swords with MP2K.
struct PartsOfGame
{
    std::string_view code;
    std::vector<PartSet> sets;
};

const std::vector<PartsOfGame>& GamesWithParts()
{
    static const std::vector<PartsOfGame> kGames = {
        {"AZLE", {{FindRd2, "A Link to the Past"}, {FindMp2k, "Four Swords"}}},
    };
    return kGames;
}

} // namespace

std::unique_ptr<Driver> FindDriver(const agblib::agb::Rom& rom, std::string& error)
{
    error.clear();
    for (const Find find :
         {FindAlphaDream, FindLogikState, FindRd1, FindRd2, FindQuintet, FindMp2k, FindIte, FindBrownie})
    {
        std::unique_ptr<Driver> driver = find(rom, error);
        if (driver || !error.empty())
        {
            return driver;
        }
    }

    error =
        "no sound driver that gsf rips found: AlphaDream's, Brownie Brown's, ITE Media's, Logik State's, Nintendo "
        "R&D1's, Nintendo R&D2's, Quintet's or MP2K";
    return nullptr;
}

std::vector<FoundDriver> FindDrivers(const agblib::agb::Rom& rom, std::string& error)
{
    std::vector<FoundDriver> drivers;
    const std::string code = rom.GameCode();
    for (const PartsOfGame& game : GamesWithParts())
    {
        if (game.code != code)
        {
            continue;
        }

        for (const PartSet& set : game.sets)
        {
            error.clear();
            std::unique_ptr<Driver> driver = set.find(rom, error);
            if (!driver)
            {
                if (error.empty())
                {
                    error = std::string("the set ") + set.name + " has no driver that gsf rips";
                }

                return {};
            }

            drivers.push_back({std::move(driver), set.name});
        }

        return drivers;
    }

    if (std::unique_ptr<Driver> driver = FindDriver(rom, error))
    {
        drivers.push_back({std::move(driver), {}});
    }

    return drivers;
}

} // namespace gsf::rip
