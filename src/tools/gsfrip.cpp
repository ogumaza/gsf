// SPDX-License-Identifier: MIT

// gsfrip: rips a Game Boy Advance game's music into a GSF set: one .gsflib, one .minigsf for each song, and with
// --effects for each sound effect, and a playlist. The library is the game image with a stub that starts the song whose
// number the mini stores. The game's sound driver plays the song.
//
// usage: gsfrip [options] <image> [<image> ...]

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <regex>
#include <set>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "agblib/agb/rom.h"
#include "seqlib/file_name.h"
#include "xsflib/cli/length_analysis.h"
#include "xsflib/cli/parallel.h"
#include "xsflib/cli/rip.h"
#include "xsflib/files.h"
#include "xsflib/rip/file_names.h"
#include "xsflib/rip/length_tags.h"
#include "xsflib/rip/tool.h"

#include "rip/cartridge.h"
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
    "inside that folder. With --effects, the set has the game's sound effects too, after the music,\n"
    "for the games whose sound effects gsfrip supports.\n"
    "\n"
    "Drivers: AlphaDream's, Brownie Brown's, Engine Software's, Frontline Studios' MOD and XM-like\n"
    "players, ITE Media's, Konami's, Logik State's, Nintendo R&D1's, Nintendo R&D2's, Quintet's,\n"
    "Webfoot Technologies' and MP2K.\n"
    "\n"
    "options:\n"
    "  -o, --output DIR     put the rips in DIR\n"
    "  --list               list the songs of each set, then stop\n"
    "  --only REGEX         rip only the songs whose name, such as \"Song 12\", or title matches\n"
    "  --effects            rip the sound effects too\n"
    "  --bgm                rip only the music (the default)\n"
    "  --no-length          skip the length analysis and omit length and fade tags\n"
    "  --no-trim            keep the whole image in the library\n"
    "  --margin S           seconds of play after a song's fade that the coverage still covers\n"
    "                       (default 10)\n"
    "  --jobs N             the number of songs that run at once (default: the number of CPU threads)\n"
    "  --game NAME, --artist NAME, --year DATE, --copyright TEXT\n"
    "                       tags (--game defaults to the image's name)\n"
    "  --by NAME, --tagger NAME\n"
    "                       the gsfby and tagger tags, written only when given\n"
    "  --version            show the version\n"
    "  -h, --help           show this help\n";

constexpr char kToolName[] = "gsfrip";
constexpr char kPlaylist[] = "!playlist.m3u";

struct Options : xsflib::cli::RipOptions
{
    bool effects = false;
    bool trim = true;
    double margin = 10.0;
    std::string tagger;
};

int ParseOption(const std::string& option, const std::function<std::string()>& next, Options& o)
{
    if (option == "--effects")
    {
        o.effects = true;
    }
    else if (option == "--no-trim")
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

// A song to rip and what its runs found. `index` is the number of its mini (xsflib::cli::WriteMinis).
struct Job
{
    uint32_t song = 0;
    uint32_t index = 0;
    bool effect = false;
    std::string label;
    std::optional<xsflib::rip::Timing> timing;
    std::optional<long long> last_sound_ms;
};

// The song's title from the game's tags, or "Song N", or "Effect N" for a sound effect.
std::string SongTitle(const gsf::rip::Driver& driver, int song)
{
    const gsf::rip::GameTags* tags = driver.Tags();
    if (tags && size_t(song) < tags->titles.size() && !tags->titles[size_t(song)].empty())
    {
        return tags->titles[size_t(song)];
    }

    return (driver.HasEffect(song) ? "Effect " : "Song ") + std::to_string(song);
}

// The jobs: the songs that --only picks, by "Song N" or by title, in the order that the game's tags give. Songs that
// the order leaves out follow, by number. With --effects, the sound effects follow the music in the same order.
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

    const bool effects = o.effects && !o.bgm;
    std::stable_partition(order.begin(), order.end(), [&](int song) { return driver.HasSong(song); });
    std::vector<Job> jobs;
    for (const int song : order)
    {
        const std::string label = SongTitle(driver, song);
        const std::string number = "Song " + std::to_string(song);
        const bool music = driver.HasSong(song);
        const bool effect = !music && effects && driver.HasEffect(song);
        if ((music || effect) && (std::regex_search(number, o.only) || std::regex_search(label, o.only)))
        {
            Job job;
            job.song = uint32_t(song);
            job.effect = effect;
            job.label = label;
            jobs.push_back(std::move(job));
        }
    }

    return jobs;
}

// Whether the stub of `driver` can start any sound effect.
bool HasAnyEffect(const gsf::rip::Driver& driver)
{
    for (int song = 0; song < driver.SongCount(); song++)
    {
        if (driver.HasEffect(song))
        {
            return true;
        }
    }

    return false;
}

void ListSongs(const gsf::rip::Driver& driver)
{
    for (const std::string& line : driver.Log())
    {
        std::printf("  %s\n", line.c_str());
    }

    for (int song = 0; song < driver.SongCount(); song++)
    {
        const bool listed = driver.HasSong(song) || driver.HasEffect(song);
        std::printf("%4d  %s\n", song, listed ? SongTitle(driver, song).c_str() : "(empty)");
    }

    std::fflush(stdout);
}

// Writes the library, the minis and the playlist into `out_dir`. The library has no tags. Each mini has `common` and
// its title, length and fade. The minis are numbered in the order of `jobs`, with two digits or as many as the count
// requires, and named after their titles. Returns 1 if a file can't be written, or 0.
int WriteSet(const gsf::rip::SetImage& set, const std::vector<Job>& jobs, const std::string& base,
             const xsflib::psf::Tags& common, const fs::path& out_dir, xsflib::cli::FileNameRegistries& names)
{
    xsflib::rip::FileNames& out_names = xsflib::cli::NamesIn(names, out_dir, ".gsflib", ".minigsf");
    const std::optional<std::string> library_name =
        xsflib::cli::WriteLibrary(out_dir, out_names, base, gsf::rip::LibraryFile(set, {}));
    if (!library_name)
    {
        return 1;
    }

    // A song that ends gets the last sound of its profile as its length, where the profile gives one.
    std::vector<Job> minis = jobs;
    for (size_t i = 0; i < minis.size(); i++)
    {
        minis[i].index = uint32_t(i + 1);
        if (minis[i].timing)
        {
            minis[i].timing = gsf::rip::TaggedTiming(*minis[i].timing, minis[i].last_sound_ms);
        }
    }

    // The conversions' analyses that give the lengths render no samples. They give no first sample. The tags count from
    // the start of the analysis: the song's start.
    std::vector<fs::path> paths;
    xsflib::cli::MiniSettings settings;
    settings.title_for_file = [](const std::string& label)
    {
        return seqlib::SafeFileName(label);
    };
    settings.paths = &paths;
    const auto build = [&library_name](const Job& job, const xsflib::psf::Tags& tags)
    {
        return gsf::rip::MiniFile(job.song, *library_name, tags);
    };
    if (xsflib::cli::WriteMinis(out_dir, out_names, minis, common, xsflib::rip::LengthFrom::kTrackStart, settings,
                                build) < 0)
    {
        return 1;
    }

    std::string playlist;
    for (const fs::path& path : paths)
    {
        playlist += xsflib::Utf8(path.filename()) + "\n";
    }

    const std::vector<uint8_t> list(playlist.begin(), playlist.end());
    if (!xsflib::rip::WriteFile(out_dir / kPlaylist, list))
    {
        std::fprintf(stderr, "error: can't write %s\n", kPlaylist);
        return 1;
    }

    return 0;
}

// Completes the reads of `coverage` (Driver::CompleteReads) and checks that the music reads none of the bytes that the
// stub replaces in `set`. Returns 1 if it reads one, or 0.
int CompleteReadsAndCheck(const std::string& input, const gsf::rip::Driver& driver, gsf::rip::Coverage& coverage,
                          const gsf::rip::SetImage& set)
{
    driver.CompleteReads(coverage.reads);
    if (auto problem = gsf::rip::CheckReplacedBytes(set, coverage.reads))
    {
        std::fprintf(stderr, "error: %s: %s\n", input.c_str(), problem->c_str());
        return 1;
    }

    return 0;
}

// Trims the set to the bytes that the songs' runs read: `coverage`, the union of their completed runs. The model runs
// no timed DMA. A trim that leaves out a byte of a timed DMA's first transfer from the cartridge fails: the set then
// plays wrongly. What such a DMA reads later depends on the order of the console's interrupts. The trim can't see that
// order. A rip warns of those reads unless the driver keeps their bytes. Returns 1 if the trim fails, or 0.
int TrimSet(const std::string& input, const gsf::rip::Driver& driver, const gsf::rip::Coverage& coverage,
            gsf::rip::SetImage& set)
{
    const std::set<gsf::rip::TimedDma>& dmas = coverage.dmas;
    if (auto problem = gsf::rip::CheckTimedDmas(set, coverage.reads, dmas))
    {
        std::fprintf(stderr, "error: %s: %s. Rip it with --no-trim.\n", input.c_str(), problem->c_str());
        return 1;
    }

    std::fprintf(stderr, "timed DMAs that read the cartridge: %llu starts, %zu distinct\n",
                 static_cast<unsigned long long>(coverage.dma_starts), dmas.size());
    if (!dmas.empty() && !driver.KeepsTimedDmaReads())
    {
        std::fprintf(
            stderr,
            "warning: %s: the music starts timed DMAs that read the cartridge, from %s. The trim can't see what "
            "they read after their first transfer. If the set plays differently from an untrimmed one, rip it "
            "with --no-trim.\n",
            input.c_str(), agblib::agb::Hex(dmas.begin()->source).c_str());
    }

    const uint64_t kept = gsf::rip::Trim(set, coverage.reads);
    std::fprintf(stderr, "the library keeps %llu of %zu bytes\n", static_cast<unsigned long long>(kept),
                 set.image.size());

    return 0;
}

// Whether a mini can store the number of each of `jobs`: 16 bits. Prints an error for the first that it can't.
bool SongNumbersFit(const std::string& input, const std::vector<Job>& jobs)
{
    for (const Job& job : jobs)
    {
        if (const std::optional<std::string> error = gsf::rip::SongNumberError(job.song))
        {
            std::fprintf(stderr, "error: %s: %s\n", input.c_str(), error->c_str());
            return false;
        }
    }

    return true;
}

// Works out each job's length, then runs it on the model with `plan` for the coverage when `cover` is set. Each run's
// map of the cartridge goes into the union when the run ends: a map for each song would use songs times the image's
// size. Returns the union.
gsf::rip::Coverage RunJobs(const agblib::agb::Rom& rom, const gsf::rip::Driver& driver, const gsf::rip::StubPlan& plan,
                           bool cover, const Options& o, std::vector<Job>& jobs)
{
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

    return coverage;
}

// Leaves out the jobs that make no sound. Returns false after an error if none is left.
bool DropSilentJobs(const std::string& input, bool effects, std::vector<Job>& jobs)
{
    const auto silent = [](const Job& job)
    {
        return job.timing && job.timing->silent;
    };
    const size_t before = jobs.size();
    std::erase_if(jobs, silent);
    if (jobs.size() < before)
    {
        std::fprintf(stderr, "%zu %s make no sound and are left out\n", before - jobs.size(),
                     effects ? "songs or sound effects" : "songs");
    }

    if (jobs.empty())
    {
        std::fprintf(stderr, "error: %s: no song makes a sound\n", input.c_str());
        return false;
    }

    return true;
}

// The tags of every mini of a rip of `input`: xsflib's tags of a rip, and the tagger. RunRipTool sets --by to the tool's
// name when it isn't given. Then the minis have no gsfby.
xsflib::psf::Tags CommonTags(const std::string& input, const Options& o)
{
    const std::string game = o.game_name.empty() ? xsflib::rip::NameOfInput(input) : o.game_name;
    xsflib::psf::Tags common = xsflib::cli::RipTags(o, game, "gsf", nullptr);
    if (o.by.empty() || o.by == kToolName)
    {
        common.erase("gsfby");
    }

    if (!o.tagger.empty())
    {
        common["tagger"] = o.tagger;
    }

    return common;
}

// Rips `jobs`, songs of `driver`, into a set in `out_dir`. Returns 1 if the rip fails, or 0.
int RipSet(const std::string& input, const agblib::agb::Rom& rom, const gsf::rip::Driver& driver, std::vector<Job> jobs,
           const Options& o, const fs::path& out_dir, xsflib::cli::FileNameRegistries& names)
{
    if (!SongNumbersFit(input, jobs))
    {
        return 1;
    }

    // A set with sound effects uses the stub that starts them. A stub inside the image requires the coverage without
    // the trim too, for the check that the music reads none of the bytes it replaces.
    const bool effects = std::any_of(jobs.begin(), jobs.end(), [](const Job& job) { return job.effect; });
    const gsf::rip::StubPlan plan = effects ? driver.PlanWithEffects() : driver.Plan();
    const bool cover = o.trim || gsf::rip::StubInImage(rom.Size(), plan);
    gsf::rip::Coverage coverage = RunJobs(rom, driver, plan, cover, o, jobs);
    if (!DropSilentJobs(input, effects, jobs))
    {
        return 1;
    }

    std::string error;
    std::optional<gsf::rip::SetImage> set =
        gsf::rip::PlaceStub(gsf::rip::CartridgeImage(rom), plan, jobs.front().song, error);
    if (!set)
    {
        std::fprintf(stderr, "error: %s: %s\n", input.c_str(), error.c_str());
        return 1;
    }

    if (cover && CompleteReadsAndCheck(input, driver, coverage, *set) != 0)
    {
        return 1;
    }

    if (o.trim && TrimSet(input, driver, coverage, *set) != 0)
    {
        return 1;
    }

    const xsflib::psf::Tags common = CommonTags(input, o);
    const std::string code = gsf::rip::ProductCode(rom.GameCode());
    const std::string base = code.empty() ? common.at("game") : code;
    std::error_code fs_error;
    fs::create_directories(out_dir, fs_error);
    if (fs_error)
    {
        std::fprintf(stderr, "error: can't create %s: %s\n", xsflib::Utf8(out_dir).c_str(), fs_error.message().c_str());
        return 1;
    }

    if (WriteSet(*set, jobs, base, common, out_dir, names) != 0)
    {
        return 1;
    }

    xsflib::rip::ReportOutput(out_dir);

    return 0;
}

// Prints the warnings of detection and of the plan of `found`'s driver, and returns the jobs of its set. Says so when
// --effects can't rip the game's sound effects.
std::vector<Job> SetJobs(const std::string& input, const gsf::rip::FoundDriver& found, const Options& o)
{
    const gsf::rip::Driver& driver = *found.driver;
    for (const std::string& line : driver.Log())
    {
        if (line.starts_with("warning:"))
        {
            std::fprintf(stderr, "  %s\n", line.c_str());
        }
    }

    if (o.effects && !o.bgm && !HasAnyEffect(driver))
    {
        const std::string set = found.set.empty() ? std::string() : " (" + found.set + ")";
        std::fprintf(stderr, "%s%s: gsfrip can't rip this game's sound effects. The set has the music only.\n",
                     input.c_str(), set.c_str());
    }

    return SongJobs(driver, o);
}

int RipInput(const std::string& input, const Options& o, xsflib::cli::FileNameRegistries& names)
{
    agblib::agb::Rom rom;
    std::string error;
    const std::vector<gsf::rip::FoundDriver> drivers = gsf::rip::LoadCartridge(input, rom, error);
    if (drivers.empty())
    {
        std::fprintf(stderr, "error: %s: %s\n", input.c_str(), error.c_str());
        return 1;
    }

    // The jobs of each set. --list lists each set's songs in their place.
    std::vector<std::vector<Job>> jobs;
    bool any = false;
    for (const gsf::rip::FoundDriver& found : drivers)
    {
        const std::string set = found.set.empty() ? std::string() : " (" + found.set + ")";
        std::fprintf(stderr, "%s%s: %s, %d songs\n", input.c_str(), set.c_str(), found.driver->Name().c_str(),
                     found.driver->SongCount());
        if (o.list)
        {
            if (!found.set.empty())
            {
                std::printf("set: %s\n", found.set.c_str());
            }

            ListSongs(*found.driver);
            continue;
        }

        jobs.push_back(SetJobs(input, found, o));
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

    // A game with more than one set gets a folder for each, named after the set.
    const fs::path out_dir = o.out_dir.empty() ? xsflib::rip::DefaultOutputDir(input, "_gsf") : fs::path(o.out_dir);
    int result = 0;
    for (size_t i = 0; i < drivers.size(); i++)
    {
        if (jobs[i].empty())
        {
            continue;
        }

        const std::string& set = drivers[i].set;
        const fs::path dir = set.empty() ? out_dir : out_dir / xsflib::PathFromUtf8(seqlib::SafeFileName(set));
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
