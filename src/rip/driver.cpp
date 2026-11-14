// SPDX-License-Identifier: MIT

#include "rip/driver.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "agblib/agb/gba.h"

namespace gsf::rip
{

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

std::unique_ptr<Driver> FindDriver(const agblib::agb::Rom& rom, std::string& error)
{
    error.clear();
    using Find = std::unique_ptr<Driver> (*)(const agblib::agb::Rom&, std::string&);
    for (const Find find : {FindAlphaDream, FindLogikState, FindRd1, FindRd2, FindQuintet, FindMp2k, FindIte})
    {
        std::unique_ptr<Driver> driver = find(rom, error);
        if (driver || !error.empty())
        {
            return driver;
        }
    }

    error =
        "no sound driver that gsf rips found: AlphaDream's, ITE Media's, Logik State's, Nintendo R&D1's, Nintendo "
        "R&D2's, Quintet's or MP2K";
    return nullptr;
}

} // namespace gsf::rip
