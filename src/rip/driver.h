// SPDX-License-Identifier: MIT

// A game's sound driver as gsf rips it: its songs, the plan of the stub that plays one, and the timing of each song.
// Each driver's code comes from agblib: its detection, the routines that the game calls and the length of its songs.

#pragma once

#include <cstdint>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "agblib/agb/rom.h"
#include "agblib/sequence_export/agb/settings.h"
#include "xsflib/rip/length.h"

#include "rip/model.h"
#include "rip/stub.h"

namespace gsf::rip
{

// A song's artist and comment where they differ from the game's. An empty field keeps the game's tag.
struct SongCredit
{
    int song = 0;
    std::string artist;
    std::string comment;
};

// The tags of a game that gsf knows: its name, the credits of its music and a title for each song. The titles name the
// screens and levels where the game plays each song, with no "/" or ":", so that a file's name can be its title. An
// empty field leaves the tag to the options.
struct GameTags
{
    std::string game;
    std::string artist;
    std::string year;      // the date of the game's first release, YYYY-MM-DD
    std::string copyright; // "Developer, Publisher"
    std::string comment;
    std::vector<std::string> titles; // by song; a song past the end or with an empty title is "Song N"
    std::vector<int> order; // the songs in the order of the set: as a player first hears them, the unused ones last
    std::vector<SongCredit> credits;
};

class Driver
{
public:
    virtual ~Driver() = default;

    // The driver's name in messages, such as "AlphaDream's driver".
    virtual std::string Name() const = 0;

    // What detection found, for --list.
    virtual const std::vector<std::string>& Log() const = 0;

    // The songs that the stub can start by number, and whether number `song` is a song.
    virtual int SongCount() const = 0;
    virtual bool HasSong(int song) const = 0;

    // The plan of the stub. Its values with `song` set take the number that the mini stores.
    virtual StubPlan Plan() const = 0;

    // Sets up a run of the plan on the model, for a driver whose model needs more than the plan, such as a stand-in for
    // a routine that reads a DMA's position.
    virtual void PrepareModel(ModelRun& run) const;

    // Marks the bytes that the console can read where the model's runs read only some of them, such as the rest of a
    // sample whose length depends on the order of interrupts. `reads` has a byte for each byte of the cartridge.
    virtual void CompleteReads(std::vector<uint8_t>& reads) const;

    // Whether CompleteReads() keeps every byte that the driver's timed DMAs read from the cartridge, such as the whole
    // of each sample that a sound FIFO's DMA reads.
    virtual bool KeepsTimedDmaReads() const;

    // The tags of the game, or null for a game that gsf has no tags for.
    virtual const GameTags* Tags() const;

    // The end of song `song`'s last sound in milliseconds, where a profile gives it, for a song that ends. The length
    // tag gives it in place of the end of the commands: the commands can go on in silence after the last sound, or
    // the samples can ring on after the commands. In the second case, the driver's SongTiming() runs to the last
    // sound, and the coverage with it.
    virtual std::optional<long long> LastSoundMs(int song) const;

    // The song's length and fade, or nothing when the length can't be worked out. `silent` is set for a song that
    // plays no notes.
    virtual std::optional<xsflib::rip::Timing> SongTiming(int song) const = 0;
};

// A looping song's fade, and the silence after a song that ends: its notes' releases.
constexpr double kFadeMs = 10000.0;
constexpr double kTailMs = 1000.0;

// The timing of a song from its conversion's report: two passes of its loop and a fade, or its end and a tail. A song
// without notes is silent. Returns nothing for a report of a song that couldn't be read.
std::optional<xsflib::rip::Timing> TimingOfReport(const agblib::sequence_export::agb::SongReport& report);

// Marks a driver's code: from the lowest of `routines` to the last byte that a run read within `tail` bytes after the
// highest. The console can take branches that the model doesn't, on state that the model has no part of, such as a
// PSG channel that its length counter stopped. Routines outside the cartridge mark nothing.
void MarkCodeSpan(std::vector<uint8_t>& reads, std::initializer_list<uint32_t> routines, uint32_t tail);

// Finds the sound driver of the game in `rom`, which has to outlive the result. Returns null and sets `error` when the
// game has none of the drivers that gsf rips, or when the driver's tables or routines can't be read.
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

// Each driver's part of FindDriver. It returns null and leaves `error` empty when the game shows no sign of the
// driver.
std::unique_ptr<Driver> FindAlphaDream(const agblib::agb::Rom& rom, std::string& error);
std::unique_ptr<Driver> FindBrownie(const agblib::agb::Rom& rom, std::string& error);
std::unique_ptr<Driver> FindLogikState(const agblib::agb::Rom& rom, std::string& error);
std::unique_ptr<Driver> FindRd1(const agblib::agb::Rom& rom, std::string& error);
std::unique_ptr<Driver> FindQuintet(const agblib::agb::Rom& rom, std::string& error);
std::unique_ptr<Driver> FindRd2(const agblib::agb::Rom& rom, std::string& error);
std::unique_ptr<Driver> FindMp2k(const agblib::agb::Rom& rom, std::string& error);
std::unique_ptr<Driver> FindIte(const agblib::agb::Rom& rom, std::string& error);

} // namespace gsf::rip
