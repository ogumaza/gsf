// SPDX-License-Identifier: MIT

// gsfrip: rips a Game Boy Advance game's music into a GSF set: one .gsflib and one .minigsf for each song. The library
// is the game's image with a stub that starts the song whose number the mini stores. The game's sound driver plays it.
//
// usage: gsfrip [options] <image> [<image> ...]

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <regex>
#include <set>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "agblib/agb/rom.h"
#include "xsflib/ascii.h"
#include "xsflib/cli/length_analysis.h"
#include "xsflib/cli/parallel.h"
#include "xsflib/cli/rip.h"
#include "xsflib/files.h"
#include "xsflib/rip/file_names.h"
#include "xsflib/rip/tool.h"

#include "rip/driver.h"
#include "rip/rip.h"

namespace
{

namespace fs = std::filesystem;

constexpr char kUsage[] =
    "usage: gsfrip [options] <image> [<image> ...]\n"
    "\n"
    "Rips each Game Boy Advance image into a .gsflib, a .minigsf for each song and a playlist,\n"
    "!playlist.m3u, in a folder next to it that's named after it with \"_gsf\" added. gsfrip finds the\n"
    "game's sound driver and adds a stub that sets up the driver as the game does and starts the song\n"
    "whose number the mini stores. Each song plays on a model of the console for its length analysis\n"
    "and for the coverage that trims the library to the bytes that the music reads. A game with a\n"
    "driver for each part, such as A Link to the Past & Four Swords, gets a folder for each part's set\n"
    "inside that folder.\n"
    "\n"
    "Drivers: AlphaDream's, Brownie Brown's, ITE Media's, Logik State's, Nintendo R&D1's, Nintendo R&D2's,\n"
    "Quintet's and MP2K.\n"
    "\n"
    "options:\n"
    "  -o, --output DIR     put the rips in DIR\n"
    "  --list               list the songs of each set, then stop\n"
    "  --only REGEX         rip only the songs whose name, such as \"Song 12\", or title matches\n"
    "  --no-length          skip the length analysis and omit length and fade tags\n"
    "  --no-trim            keep the whole image in the library\n"
    "  --margin S           seconds of play after a song's fade that the coverage still covers\n"
    "                       (default 10)\n"
    "  --jobs N             the number of songs that run at once (default: the number of CPU threads)\n"
    "  --game NAME, --artist NAME, --year DATE, --copyright TEXT\n"
    "                       tags (for a game that gsf knows, they default to its tags; --game\n"
    "                       otherwise defaults to the image's name)\n"
    "  --by NAME, --tagger NAME\n"
    "                       the gsfby and tagger tags, written only when given\n"
    "  --version            show the version\n"
    "  -h, --help           show this help\n";

constexpr char kToolName[] = "gsfrip";
constexpr char kPlaylist[] = "!playlist.m3u";

struct Options : xsflib::cli::RipOptions
{
    bool trim = true;
    double margin = 10.0;
    std::string tagger;
};

int ParseOption(const std::string& option, const std::function<std::string()>& next, Options& o)
{
    if (option == "--no-trim")
    {
        o.trim = false;
    }
    else if (option == "--tagger")
    {
        o.tagger = next();
    }
    else if (option == "--margin")
    {
        const std::string value = next();
        char* end = nullptr;
        const double margin = std::strtod(value.c_str(), &end);
        if (value.empty() || *end != '\0' || !(margin >= 0.0 && margin <= 3600.0))
        {
            std::fprintf(stderr, "error: --margin takes seconds from 0 to 3600, not '%s'\n", value.c_str());
            return 2;
        }

        o.margin = margin;
    }
    else
    {
        return -1;
    }

    return 0;
}

// A song to rip and what its runs found.
struct Job
{
    uint32_t song = 0;
    std::string label;
    std::optional<xsflib::rip::Timing> timing;
    std::optional<long long> last_sound_ms;
};

// The song's title from the game's tags, or "Song N".
std::string SongTitle(const gsf::rip::Driver& driver, int song)
{
    const gsf::rip::GameTags* tags = driver.Tags();
    if (tags && size_t(song) < tags->titles.size() && !tags->titles[size_t(song)].empty())
    {
        return tags->titles[size_t(song)];
    }

    return "Song " + std::to_string(song);
}

// The jobs: the songs that --only picks, by "Song N" or by title, in the order that the game's tags give. Songs that
// the order leaves out follow, by number.
std::vector<Job> SongJobs(const gsf::rip::Driver& driver, const Options& o)
{
    std::vector<int> order;
    if (const gsf::rip::GameTags* tags = driver.Tags())
    {
        order = tags->order;
    }

    for (int song = 0; song < driver.SongCount(); song++)
    {
        if (std::find(order.begin(), order.end(), song) == order.end())
        {
            order.push_back(song);
        }
    }

    std::vector<Job> jobs;
    for (const int song : order)
    {
        const std::string label = SongTitle(driver, song);
        const std::string number = "Song " + std::to_string(song);
        if (driver.HasSong(song) && (std::regex_search(number, o.only) || std::regex_search(label, o.only)))
        {
            Job job;
            job.song = uint32_t(song);
            job.label = label;
            jobs.push_back(std::move(job));
        }
    }

    return jobs;
}

void ListSongs(const gsf::rip::Driver& driver)
{
    for (const std::string& line : driver.Log())
    {
        std::printf("  %s\n", line.c_str());
    }

    for (int song = 0; song < driver.SongCount(); song++)
    {
        std::printf("%4d  %s\n", song, driver.HasSong(song) ? SongTitle(driver, song).c_str() : "(empty)");
    }
    std::fflush(stdout);
}

// The mini names that the rips so far wrote, by folder, in lower case.
std::map<std::string, std::set<std::string>> g_mini_names;

// Writes the library, which has no tags, the minis, each with `common` and its title, length and fade, and the
// playlist into `out_dir`. The minis are numbered in the order of `jobs`, with two digits or as many as the count
// needs, and named after their titles. Returns 1 if a file can't be written, or 0.
int WriteSet(const gsf::rip::SetImage& set, const std::vector<Job>& jobs, const std::string& base,
             const xsflib::psf::Tags& common, const std::vector<gsf::rip::SongCredit>& credits, const fs::path& out_dir,
             xsflib::cli::FileNameRegistries& names)
{
    xsflib::rip::FileNames& out_names = xsflib::cli::NamesIn(names, out_dir, ".gsflib", ".minigsf");
    const std::optional<std::string> library_name =
        xsflib::cli::WriteLibrary(out_dir, out_names, base, gsf::rip::LibraryFile(set, {}));
    if (!library_name)
    {
        return 1;
    }

    const int digits = std::max(2, int(std::to_string(jobs.size()).size()));
    std::set<std::string>& taken = g_mini_names[xsflib::AsciiLower(xsflib::Utf8(fs::absolute(out_dir)))];
    std::string playlist;
    for (size_t i = 0; i < jobs.size(); i++)
    {
        const Job& job = jobs[i];
        xsflib::psf::Tags tags = common;
        tags["title"] = job.label;
        for (const gsf::rip::SongCredit& credit : credits)
        {
            if (credit.song != int(job.song))
            {
                continue;
            }

            for (auto [name, value] : {std::pair{"artist", &credit.artist}, std::pair{"comment", &credit.comment}})
            {
                if (!value->empty())
                {
                    tags[name] = *value;
                }
            }
        }

        std::string length;
        if (job.timing)
        {
            const gsf::rip::LengthTags times = gsf::rip::TagsOfTiming(*job.timing, job.last_sound_ms);
            tags["length"] = length = times.length;
            tags["fade"] = times.fade;
        }

        char number[16];
        std::snprintf(number, sizeof number, "%0*zu ", digits, i + 1);
        const std::string stem = number + gsf::rip::FileTitle(job.label);
        std::string name = stem + ".minigsf";
        for (int n = 2; !taken.insert(xsflib::AsciiLower(name)).second; n++)
        {
            name = stem + "_" + std::to_string(n) + ".minigsf";
        }

        if (!xsflib::rip::WriteFile(out_dir / xsflib::PathFromUtf8(name),
                                    gsf::rip::MiniFile(set, job.song, *library_name, tags)))
        {
            std::fprintf(stderr, "error: can't write %s\n", name.c_str());
            return 1;
        }

        std::fprintf(stderr, "%s%s%s\n", name.c_str(), length.empty() ? "" : "  length ", length.c_str());
        playlist += name + "\n";
    }

    const std::vector<uint8_t> list(playlist.begin(), playlist.end());
    if (!xsflib::rip::WriteFile(out_dir / kPlaylist, list))
    {
        std::fprintf(stderr, "error: can't write %s\n", kPlaylist);
        return 1;
    }

    return 0;
}

// Trims the set to the bytes that the songs' runs read: `coverage`, the union of their runs. The model runs no timed
// DMA. A trim that leaves out a byte of a timed DMA's first transfer from the cartridge fails: the set then plays
// wrongly for certain. What such a DMA reads after that depends on the order of the console's interrupts, which the
// trim can't see, so a rip warns of it, unless the driver keeps those bytes. Returns 1 if the trim fails, or 0.
int TrimSet(const std::string& input, const gsf::rip::Driver& driver, gsf::rip::Coverage& coverage,
            gsf::rip::SetImage& set)
{
    std::vector<uint8_t>& reads = coverage.reads;
    const std::set<gsf::rip::TimedDma>& dmas = coverage.dmas;
    const uint64_t dma_starts = coverage.dma_starts;
    driver.CompleteReads(reads);
    if (auto problem = gsf::rip::CheckStubRoom(set, reads))
    {
        std::fprintf(stderr, "error: %s: %s\n", input.c_str(), problem->c_str());
        return 1;
    }

    if (auto problem = gsf::rip::CheckTimedDmas(set, reads, dmas))
    {
        std::fprintf(stderr, "error: %s: %s. Rip it with --no-trim.\n", input.c_str(), problem->c_str());
        return 1;
    }

    std::fprintf(stderr, "timed DMAs that read the cartridge: %llu starts, %zu distinct\n",
                 static_cast<unsigned long long>(dma_starts), dmas.size());
    if (!dmas.empty() && !driver.KeepsTimedDmaReads())
    {
        std::fprintf(
            stderr,
            "warning: %s: the music starts timed DMAs that read the cartridge, from %s. The trim can't see what "
            "they read after their first transfer. If the set plays differently from an untrimmed one, rip it "
            "with --no-trim.\n",
            input.c_str(), agblib::agb::Hex(dmas.begin()->source).c_str());
    }

    const uint64_t kept = gsf::rip::Trim(set, reads);
    std::fprintf(stderr, "the library keeps %llu of %zu bytes\n", static_cast<unsigned long long>(kept),
                 set.image.size());
    return 0;
}

// Rips `jobs`, songs of `driver`, into a set in `out_dir`. Returns 1 if the rip fails, or 0.
int RipSet(const std::string& input, const agblib::agb::Rom& rom, const gsf::rip::Driver& driver, std::vector<Job> jobs,
           const Options& o, const fs::path& out_dir, xsflib::cli::FileNameRegistries& names)
{
    std::string error;
    // Each song's length, then its run on the model for the coverage. Each run's map of the cartridge goes into the
    // union when the run ends: a map for each song would take songs times the image's size. A stub inside the image
    // needs the coverage without the trim too, for the check that the music reads none of the bytes it replaces.
    const gsf::rip::StubPlan plan = driver.Plan();
    const bool cover = o.trim || gsf::rip::StubInImage(rom.Size(), plan);
    gsf::rip::Coverage coverage;
    std::mutex coverage_mutex;
    const auto analyze = [&](Job& job)
    {
        if (o.lengths)
        {
            job.timing = driver.SongTiming(int(job.song));
            job.last_sound_ms = driver.LastSoundMs(int(job.song));
        }

        if (cover && !(job.timing && job.timing->silent))
        {
            const gsf::rip::Coverage run =
                gsf::rip::CoverSong(rom, driver, plan, job.song, gsf::rip::CoverageFrames(job.timing, o.margin));
            const std::lock_guard lock(coverage_mutex);
            gsf::rip::AddReads(coverage.reads, run.reads);
            coverage.dmas.insert(run.dmas.begin(), run.dmas.end());
            coverage.dma_starts += run.dma_starts;
        }

        return o.lengths;
    };
    xsflib::cli::AnalysisSettings analysis;
    analysis.progress = "ran";
    xsflib::cli::AnalyzeLengths(jobs, o.jobs, analyze, analysis);

    // A song that plays no notes gets no mini.
    const auto silent = [](const Job& job)
    {
        return job.timing && job.timing->silent;
    };
    const size_t before = jobs.size();
    std::erase_if(jobs, silent);
    if (jobs.size() < before)
    {
        std::fprintf(stderr, "%zu songs play no notes and are left out\n", before - jobs.size());
    }

    if (jobs.empty())
    {
        std::fprintf(stderr, "error: %s: no song plays a note\n", input.c_str());
        return 1;
    }

    const std::vector<uint8_t> image(rom.Ptr(agblib::agb::kRomBase), rom.Ptr(agblib::agb::kRomBase) + rom.Size());
    std::optional<gsf::rip::SetImage> set = gsf::rip::PlaceStub(image, plan, jobs.front().song, error);
    if (!set)
    {
        std::fprintf(stderr, "error: %s: %s\n", input.c_str(), error.c_str());
        return 1;
    }

    if (o.trim && TrimSet(input, driver, coverage, *set) != 0)
    {
        return 1;
    }

    if (!o.trim && cover)
    {
        driver.CompleteReads(coverage.reads);
        if (auto problem = gsf::rip::CheckStubRoom(*set, coverage.reads))
        {
            std::fprintf(stderr, "error: %s: %s\n", input.c_str(), problem->c_str());
            return 1;
        }
    }

    // The options' tags, or the game's where an option is empty.
    Options tagged = o;
    std::string comment;
    std::vector<gsf::rip::SongCredit> credits;
    if (const gsf::rip::GameTags* tags = driver.Tags())
    {
        // --artist replaces the songs' artists too. An empty artist keeps the common one, and the comments stay.
        credits = tags->credits;
        if (!o.artist.empty())
        {
            for (gsf::rip::SongCredit& credit : credits)
            {
                credit.artist.clear();
            }
        }

        for (auto [option, tag] :
             {std::pair{&tagged.game_name, &tags->game}, std::pair{&tagged.artist, &tags->artist},
              std::pair{&tagged.year, &tags->year}, std::pair{&tagged.copyright, &tags->copyright}})
        {
            if (option->empty())
            {
                *option = *tag;
            }
        }
        comment = tags->comment;
    }

    const std::string game = tagged.game_name.empty() ? xsflib::rip::NameOfInput(input) : tagged.game_name;
    xsflib::psf::Tags common;
    common["game"] = game;
    for (auto [name, value] : {std::pair{"artist", &tagged.artist}, std::pair{"year", &tagged.year},
                               std::pair{"copyright", &tagged.copyright}, std::pair{"comment", &comment},
                               std::pair{"tagger", &tagged.tagger}})
    {
        if (!value->empty())
        {
            common[name] = *value;
        }
    }

    // RunRipTool sets --by to the tool's name when it isn't given. Then the minis have no gsfby.
    if (!tagged.by.empty() && tagged.by != kToolName)
    {
        common["gsfby"] = tagged.by;
    }
    common["utf8"] = "1";

    const std::string code = gsf::rip::ProductCode(rom.GameCode());
    const std::string base = code.empty() ? game : code;
    std::error_code fs_error;
    fs::create_directories(out_dir, fs_error);
    if (fs_error)
    {
        std::fprintf(stderr, "error: can't create %s: %s\n", xsflib::Utf8(out_dir).c_str(), fs_error.message().c_str());
        return 1;
    }

    if (WriteSet(*set, jobs, base, common, credits, out_dir, names) != 0)
    {
        return 1;
    }

    xsflib::rip::ReportOutput(out_dir);
    return 0;
}

int RipInput(const std::string& input, const Options& o, xsflib::cli::FileNameRegistries& names)
{
    agblib::agb::Rom rom;
    std::string error;
    if (!rom.Load(xsflib::Utf8(fs::absolute(xsflib::PathFromUtf8(input))), error))
    {
        std::fprintf(stderr, "error: %s: %s\n", input.c_str(), error.c_str());
        return 1;
    }

    if (rom.FromGsf())
    {
        std::fprintf(stderr, "error: %s: a GSF set, not a cartridge image\n", input.c_str());
        return 1;
    }

    const std::vector<gsf::rip::FoundDriver> drivers = gsf::rip::FindDrivers(rom, error);
    if (drivers.empty())
    {
        std::fprintf(stderr, "error: %s: %s\n", input.c_str(), error.c_str());
        return 1;
    }

    // A game with more than one set gets a folder for each, named after the set.
    std::vector<std::vector<Job>> jobs;
    bool any = false;
    for (const gsf::rip::FoundDriver& found : drivers)
    {
        const gsf::rip::Driver& driver = *found.driver;
        const std::string set = found.set.empty() ? std::string() : " (" + found.set + ")";
        std::fprintf(stderr, "%s%s: %s, %d songs\n", input.c_str(), set.c_str(), driver.Name().c_str(),
                     driver.SongCount());
        if (o.list)
        {
            if (!found.set.empty())
            {
                std::printf("set: %s\n", found.set.c_str());
            }
            ListSongs(driver);
            continue;
        }

        jobs.push_back(SongJobs(driver, o));
        any = any || !jobs.back().empty();
    }

    if (o.list)
    {
        return 0;
    }

    if (!any)
    {
        std::fprintf(stderr, "error: %s: no song to rip\n", input.c_str());
        return 1;
    }

    const fs::path out_dir = o.out_dir.empty() ? xsflib::rip::DefaultOutputDir(input, "_gsf") : fs::path(o.out_dir);
    int result = 0;
    for (size_t i = 0; i < drivers.size(); i++)
    {
        if (jobs[i].empty())
        {
            continue;
        }

        const std::string& set = drivers[i].set;
        const fs::path dir = set.empty() ? out_dir : out_dir / xsflib::PathFromUtf8(gsf::rip::FileTitle(set));
        if (RipSet(input, rom, *drivers[i].driver, std::move(jobs[i]), o, dir, names) != 0)
        {
            result = 1;
        }
    }

    return result;
}

} // namespace

int main(int argc, char** argv)
{
    xsflib::cli::RipTool<Options> tool;
    tool.name = kToolName;
    tool.version = GSF_VERSION;
    tool.usage = kUsage;
    tool.modes = false;
    tool.archives = false;
    tool.parse_option = ParseOption;
    tool.rip_input = RipInput;

    return xsflib::cli::RunRipTool(tool, argc, argv);
}
