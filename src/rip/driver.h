// SPDX-License-Identifier: MIT

// A sound driver as gsf rips it: the songs, the plan of the stub that plays a song, and the timing of each song. agblib
// provides the code of each driver: the detection, the routines that the game calls and the lengths of the songs.

#pragma once

#include <cstdint>
#include <functional>
#include <initializer_list>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "agblib/agb/rom.h"
#include "agblib/sequence_export/agb/settings.h"
#include "xsflib/rip/length.h"

#include "rip/model.h"
#include "rip/stub.h"

namespace gsf::rip
{

// The titles and the order of a game's songs that a driver works out from the game's data. An empty title leaves the
// song named by its number.
struct GameTags
{
    std::vector<std::string> titles; // by song; a song past the end or with an empty title is "Song N" or "Effect N"
    std::vector<int> order;          // the songs in the order of the set; the songs it leaves out follow by number
};

// Adds songs `first` to `end` - 1 to the order of `tags`: those that `used` accepts in number order, then the others,
// titled "<kind> N (Unused)".
void AddSongsUnusedLast(GameTags& tags, int first, int end, const std::string& kind,
                        const std::function<bool(int song)>& used);

class Driver
{
public:
    virtual ~Driver() = default;

    // The driver's name in messages, such as "AlphaDream's driver".
    virtual std::string Name() const = 0;

    // What detection found, for --list.
    virtual const std::vector<std::string>& Log() const = 0;

    // The songs and sound effects that the stub can start by number, and whether number `song` is a song of the music.
    virtual int SongCount() const = 0;
    virtual bool HasSong(int song) const = 0;

    // Whether number `song` is a sound effect that the stub can start. --effects adds these to a rip. The default is
    // none.
    virtual bool HasEffect(int song) const;

    // The plan of the stub. Its values with `song` set take the number that the mini stores.
    virtual StubPlan Plan() const = 0;

    // The plan of a stub that starts the sound effects too. The default is Plan(), for a driver whose music plan starts
    // them. A driver overrides it where its sound effects take another call. A rip of the music alone then keeps the
    // music stub.
    virtual StubPlan PlanWithEffects() const;

    // Sets up a run of the plan on the model, for a driver whose model run requires more than the plan, such as a
    // stand-in for a routine that reads the position of a DMA. CoverSong() calls it after the start of the plan has
    // run. A setting that it makes, such as a held vertical count, then can't stall a wait in the start: the init of
    // MP2K waits for line 159.
    virtual void PrepareModel(ModelRun& run) const;

    // Marks the bytes that the console can read where the model's runs read only some of them, such as the rest of a
    // sample whose length depends on the order of interrupts. `reads` has a byte for each byte of the cartridge.
    virtual void CompleteReads(std::vector<uint8_t>& reads) const;

    // Whether CompleteReads() keeps every byte that the driver's timed DMAs read from the cartridge, such as the whole
    // of each sample that a sound FIFO's DMA reads.
    virtual bool KeepsTimedDmaReads() const;

    // The titles and the order of the game's songs, or null for the default: "Song N", the music by number, then the
    // sound effects.
    virtual const GameTags* Tags() const;

    // The end of song `song`'s last sound in milliseconds, where a profile gives it, for a song that ends. The length
    // tag gives it in place of the end of the commands: the commands can go on in silence after the last sound, or the
    // samples can ring on after the commands. In the second case, the driver's SongTiming() runs to the last sound, and
    // the coverage with it.
    virtual std::optional<long long> LastSoundMs(int song) const;

    // The song's length and fade, or nothing when the length can't be worked out. `silent` is set for a song that makes
    // no sound.
    virtual std::optional<xsflib::rip::Timing> SongTiming(int song) const = 0;
};

// A looping song's fade: the fade of a loop in xsflib's length rules.
constexpr double kFadeMs = xsflib::rip::LengthRules{}.fade_ms;

// The timing of a song from its conversion's report: two passes of its loop, with the last pass, and a fade; or its
// end. A loop whose two passes would start more than kShortFirstPassMs before the analysis gets no pass, and a loop
// that would start less early starts at 0. A song that the report calls silent makes no sound. Returns nothing for a
// report of a song that couldn't be read.
std::optional<xsflib::rip::Timing> TimingOfReport(const agblib::sequence_export::agb::SongReport& report);

// Marks the code of a driver: from the lowest of `routines` to the last byte that a run read within `tail` bytes after
// the highest. The console can take branches that the model doesn't take, on state that the model doesn't have, such as
// a PSG channel that its length counter stopped. Nothing is marked when the lowest routine is outside the cartridge.
void MarkCodeSpan(std::vector<uint8_t>& reads, std::initializer_list<uint32_t> routines, uint32_t tail);

// A run of bytes of the cartridge, by its address.
struct RomSpan
{
    uint32_t address = 0;
    uint32_t size = 0;
};

// The spans of `spans`, each an address and a size, in order of address.
std::vector<RomSpan> SpansOf(const std::set<std::pair<uint32_t, uint32_t>>& spans);

// Whether `reads` marks any byte of `span`.
bool AnyRead(const std::vector<uint8_t>& reads, const RomSpan& span);

// Marks every byte of `span`, a span of the cartridge. `reads` grows to the end of the span.
void MarkSpan(std::vector<uint8_t>& reads, const RomSpan& span);

// Marks the whole of each span of `spans` of which `reads` marks any byte: a sample or a record that the console can
// read more of than the model's runs did. `reads` grows to the end of a span that runs past it.
void KeepWholeSpans(std::vector<uint8_t>& reads, std::span<const RomSpan> spans);

// Appends the spans of the instrument table at `table` to `spans`: each entry, then the spans that `instrument` returns
// for the address in the entry (0 for an entry outside the cartridge). The entries run until `instrument` returns none,
// at most 256 of them: a cell's instrument is a byte. With `skip_empty_first`, an entry 0 without spans is passed over,
// for a player whose instrument 0 keeps the last.
void AddInstrumentSpans(const agblib::agb::Rom& rom, uint32_t table, bool skip_empty_first,
                        const std::function<std::vector<RomSpan>(uint32_t record)>& instrument,
                        std::vector<RomSpan>& spans);

// The end of the last sound of a song, where a profile gives it: the number of the song, and the end in milliseconds
// from the start of the song.
struct SongEnd
{
    int song = 0;
    long long ms = 0;
};

// The end that `ends` gives song `song`, or nothing.
std::optional<long long> EndOfSong(std::span<const SongEnd> ends, int song);

// The profile in `profiles` of the game with the game code `code` and the software version `version` whose member
// `routine` is `address`, or null for a game that no profile covers. A profile names the routines that the game calls.
// The match on the address that detection finds ties those routines to this image.
template <typename Profile>
const Profile* FindProfile(std::span<const std::type_identity_t<Profile>> profiles, std::string_view code,
                           uint8_t version, uint32_t Profile::* routine, uint32_t address)
{
    for (const Profile& profile : profiles)
    {
        if (profile.code == code && profile.version == version && profile.*routine == address)
        {
            return &profile;
        }
    }

    return nullptr;
}

// Returns the profile in `profiles` for the game code `code`, or null for a game that no profile covers.
template <typename Profile>
const Profile* FindProfile(std::span<const std::type_identity_t<Profile>> profiles, std::string_view code)
{
    for (const Profile& profile : profiles)
    {
        if (profile.code == code)
        {
            return &profile;
        }
    }

    return nullptr;
}

// The error of `driver`, such as "Konami's driver", for the game `game` that no profile covers. `game` is the game
// code, or what GameWithRoutine() gives.
std::string NoProfileError(const std::string& driver, const std::string& game);

// The game as FindProfile() matches it, for NoProfileError(): "<code> (version <version>) with its <what> at
// <address>".
std::string GameWithRoutine(const std::string& code, uint8_t version, const std::string& what, uint32_t address);

// Finds the sound driver of the game in `rom`. `rom` has to outlive the result. Returns null and sets `error` when the
// game has none of the drivers that gsf rips, or when the tables or the routines of the driver can't be read.
std::unique_ptr<Driver> FindDriver(const agblib::agb::Rom& rom, std::string& error);

// A driver that FindDrivers() found, and its set's name when the image has more than one set: the name of the set's
// folder.
struct FoundDriver
{
    std::unique_ptr<Driver> driver;
    std::string set;
};

// Finds the sound drivers of the game in `rom`. `rom` has to outlive the result. The result is FindDriver()'s driver,
// or each driver of a game in the table of games with a driver for each part. Returns none and sets `error` as
// FindDriver() does, or when a driver of such a game can't be read.
std::vector<FoundDriver> FindDrivers(const agblib::agb::Rom& rom, std::string& error);

// The search of each driver that FindDriver() makes. Each returns null and leaves `error` empty when the game shows no
// sign of the driver.
std::unique_ptr<Driver> FindAlphaDream(const agblib::agb::Rom& rom, std::string& error);
std::unique_ptr<Driver> FindBrownie(const agblib::agb::Rom& rom, std::string& error);
std::unique_ptr<Driver> FindLogikState(const agblib::agb::Rom& rom, std::string& error);
std::unique_ptr<Driver> FindRd1(const agblib::agb::Rom& rom, std::string& error);
std::unique_ptr<Driver> FindQuintet(const agblib::agb::Rom& rom, std::string& error);
std::unique_ptr<Driver> FindRd2(const agblib::agb::Rom& rom, std::string& error);
std::unique_ptr<Driver> FindMp2k(const agblib::agb::Rom& rom, std::string& error);
std::unique_ptr<Driver> FindIte(const agblib::agb::Rom& rom, std::string& error);
std::unique_ptr<Driver> FindKonami(const agblib::agb::Rom& rom, std::string& error);
std::unique_ptr<Driver> FindWebfoot(const agblib::agb::Rom& rom, std::string& error);
std::unique_ptr<Driver> FindFrontlineMod(const agblib::agb::Rom& rom, std::string& error);
std::unique_ptr<Driver> FindFrontlineXm(const agblib::agb::Rom& rom, std::string& error);

// Webfoot Technologies' driver in a cartridge with several programs, each with a copy of the driver and a song table:
// the driver of song table `part`, in address order.
std::unique_ptr<Driver> FindWebfootPart(const agblib::agb::Rom& rom, int part, std::string& error);

} // namespace gsf::rip
