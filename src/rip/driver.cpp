// SPDX-License-Identifier: MIT

#include "rip/driver.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
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

std::vector<RomSpan> SpansOf(const std::set<std::pair<uint32_t, uint32_t>>& spans)
{
    std::vector<RomSpan> result;
    result.reserve(spans.size());
    for (const auto& [address, size] : spans)
    {
        result.push_back({address, size});
    }

    return result;
}

bool AnyRead(const std::vector<uint8_t>& reads, const RomSpan& span)
{
    const size_t first = span.address - agblib::agb::kRomBase;
    const size_t last = std::min(reads.size(), first + span.size);
    return first < last && std::any_of(
                               reads.begin() + std::ptrdiff_t(first), reads.begin() + std::ptrdiff_t(last),
                               [](uint8_t mark) { return mark != 0; });
}

void MarkSpan(std::vector<uint8_t>& reads, const RomSpan& span)
{
    const size_t first = span.address - agblib::agb::kRomBase;
    if (reads.size() < first + span.size)
    {
        reads.resize(first + span.size);
    }

    std::fill(reads.begin() + std::ptrdiff_t(first), reads.begin() + std::ptrdiff_t(first + span.size), uint8_t{1});
}

void KeepWholeSpans(std::vector<uint8_t>& reads, std::span<const RomSpan> spans)
{
    for (const RomSpan& span : spans)
    {
        if (AnyRead(reads, span))
        {
            MarkSpan(reads, span);
        }
    }
}

std::optional<long long> EndOfSong(std::span<const SongEnd> ends, int song)
{
    for (const SongEnd& end : ends)
    {
        if (end.song == song)
        {
            return end.ms;
        }
    }

    return std::nullopt;
}

std::string NoProfileError(const std::string& driver, const std::string& game)
{
    return driver + ": the game " + game + " is in no profile that gsf has, so the routines that it calls are unknown";
}

std::string GameWithRoutine(const std::string& code, uint8_t version, const std::string& what, uint32_t address)
{
    return code + " (version " + std::to_string(version) + ") with its " + what + " at " + agblib::agb::Hex(address);
}

void AddSongsUnusedLast(GameTags& tags, int first, int end, const std::string& kind,
                        const std::function<bool(int song)>& used)
{
    if (tags.titles.size() < size_t(end))
    {
        tags.titles.resize(size_t(end));
    }

    std::vector<int> unused;
    for (int song = first; song < end; song++)
    {
        if (used(song))
        {
            tags.order.push_back(song);
        }
        else
        {
            tags.titles[size_t(song)] = kind + " " + std::to_string(song) + " (Unused)";
            unused.push_back(song);
        }
    }

    tags.order.insert(tags.order.end(), unused.begin(), unused.end());
}

void AddInstrumentSpans(const agblib::agb::Rom& rom, uint32_t table, bool skip_empty_first,
                        const std::function<std::vector<RomSpan>(uint32_t record)>& instrument,
                        std::vector<RomSpan>& spans)
{
    constexpr uint32_t kMaxInstruments = 256;
    for (uint32_t i = 0; i < kMaxInstruments; i++)
    {
        const uint32_t entry = table + 4 * i;
        const std::vector<RomSpan> parts = instrument(rom.Contains(entry, 4) ? rom.U32(entry) : 0);
        if (parts.empty())
        {
            if (i == 0 && skip_empty_first)
            {
                continue;
            }
            break;
        }

        spans.push_back({entry, 4});
        spans.insert(spans.end(), parts.begin(), parts.end());
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
    const auto frames_of = [](double seconds)
    {
        return std::llround(seconds * agblib::agb::kFrameRate);
    };

    const long long frames = frames_of(report.seconds);
    if (report.loop_start < 0)
    {
        return xsflib::rip::EndAt(uint64_t(frames), frame_ms);
    }

    // The pass is the last: from the end of the first pass to the length. A report whose first pass ends at or after
    // the length gives a pass of 0. PassesAt keeps a pass only where xsflib::rip::KeepsPass does.
    const long long pass = std::max(frames - frames_of(report.loop_end), 0LL);

    return xsflib::rip::PassesAt(uint64_t(frames), uint64_t(pass), frame_ms, kFadeMs);
}

namespace
{

using Find = std::unique_ptr<Driver> (*)(const agblib::agb::Rom&, std::string&);
using FindPart = std::unique_ptr<Driver> (*)(const agblib::agb::Rom&, int, std::string&);

// A set of a game with more than one: the driver of its part of the game, and the set's name. Where one driver plays
// every part from a table of its own, `find_part` finds the driver of table `part` in place of `find`.
struct PartSet
{
    Find find = nullptr;
    const char* name = "";
    FindPart find_part = nullptr;
    int part = 0;
};

// The games with a driver for each part, by the game code in the cartridge's header. The Legend of Zelda: A Link to the
// Past & Four Swords plays A Link to the Past with Nintendo R&D2's driver, and its menus and Four Swords with MP2K. The
// cartridge of Dragon Ball Z: Buu's Fury and Dragon Ball GT: Transformation contains three programs: the menu and the
// two games. Each has a copy of Webfoot Technologies' driver and a song table.
struct PartsOfGame
{
    std::string_view code;
    std::vector<PartSet> sets;
};

const std::vector<PartsOfGame>& GamesWithParts()
{
    static const std::vector<PartsOfGame> kGames = {
        {"AZLE", {{FindRd2, "A Link to the Past"}, {FindMp2k, "Four Swords"}}},
        {"BUFE",
         {{.name = "Menu", .find_part = FindWebfootPart, .part = 0},
          {.name = "Buu's Fury", .find_part = FindWebfootPart, .part = 1},
          {.name = "GT Transformation", .find_part = FindWebfootPart, .part = 2}}},
    };
    return kGames;
}

} // namespace

std::unique_ptr<Driver> FindDriver(const agblib::agb::Rom& rom, std::string& error)
{
    error.clear();
    for (const Find find : {FindAlphaDream, FindLogikState, FindRd1, FindRd2, FindQuintet, FindMp2k, FindIte,
                            FindBrownie, FindKonami, FindWebfoot, FindFrontlineMod, FindFrontlineXm})
    {
        std::unique_ptr<Driver> driver = find(rom, error);
        if (driver || !error.empty())
        {
            return driver;
        }
    }

    error =
        "no sound driver that gsf rips found: AlphaDream's, Brownie Brown's, Frontline Studios' MOD or XM-like player, "
        "ITE Media's, Konami's, Logik State's, Nintendo R&D1's, Nintendo R&D2's, Quintet's, Webfoot Technologies' or "
        "MP2K";
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
            std::unique_ptr<Driver> driver = set.find ? set.find(rom, error) : set.find_part(rom, set.part, error);
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
