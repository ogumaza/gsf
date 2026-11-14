# gsf

gsf rips the music of Game Boy Advance games to GSF, the PSF-family format for Game Boy Advance music. A GSF set holds
the game's cartridge image, cut down to the code and data that its music reads, and a small stub that starts one song.
A player emulates the console and runs the game's sound driver, so the music plays as it does in the game.

`gsfrip` finds a game's sound driver, writes the stub for it and rips the set. gsf has no player of its own. The GSF
players built on mGBA play the sets.

## Supported drivers

`gsfrip` rips games with these sound drivers:

- AlphaDream's driver: Mario & Luigi: Superstar Saga and two Hamtaro games (Tottoko Hamtaro 4: Nijiiro Daikoushin
  Dechu and Tottoko Hamtaro: Hamu-Hamu Sports)
- ITE Media's driver: Hugo: The Evil Mirror, the US release. Each song of the game's bank is a song, and it plays at
  the music volume that the game sets for it. A song that ends plays out to its last sound. In the game, the
  interrupted music comes back after a jingle's last row and cuts the jingle short. The audio of the game's videos
  isn't ripped.
- Logik State's driver, GBAModPlay: Aero the Acro-Bat: Rascal Rival Revenge, the European and US releases; Zapper, the
  US release; and Pinball Challenge Deluxe, the European release. In Aero and Zapper, each of the game's modules is a
  song, and the songs are numbered in the order of the modules in the image. Aero's set adds the part of one module that
  the game skips. Pinball Challenge Deluxe starts its pieces at orders within its modules, and each piece is a song. A
  jingle ends where the game would go back to the music that it interrupted. The songs play at the music's volume that
  the game starts with.
- Nintendo R&D1's driver: WarioWare, Inc.: Mega Microgame$! and Rhythm Tengoku. Each entry of the song table that
  holds a song is a song, sound effects included. A song plays as the song table starts it. The games' stages can play
  it at another tempo, pitch or volume, or with some tracks silent, and the sets don't follow them.
- Nintendo R&D2's driver: Super Mario Advance 2, 3 and 4, the US releases. Each sequence of the music is a song. A
  sequence that the game plays by a music number plays as the first such number sets it up: with its voices, its
  volume and its muted tracks. In Super Mario Advance 2, the tracks that play only while Mario rides Yoshi stay silent.
- Quintet's driver: Super Robot Taisen A, D, R and J; Bouken Yuuki Plaston GP, Bouken Yuuki Densetsu Plaston Gate and
  Bouken Yuuki PlaStar World: Densetsu no Plust Gate EX; Legend of Dynamic: Goushouden; Blender Bros.; Doubutsu no
  Shima no Chobi Gurumi; Gachasute! Dyna Device Red and Blue; Robopon 2: Ring Version and Cross Version; and Magical
  Fengshen. The songs of each music file are songs, numbered one file after another. The sound effects aren't ripped.
- MP2K, the sound driver of Nintendo's SDK, which most GBA games use. Each entry of the song table is a song, and it
  plays as the driver starts it. gsf doesn't rip games that set a song up in their code before they start it, or games
  with Camelot's version of the driver, such as Golden Sun: The Lost Age. `gsfrip` names the reason.

A game with another driver gets "no sound driver that gsf rips found".

## Ripping a game

```
gsfrip game.gba
```

`gsfrip` writes the set to a folder next to the image, named after it with `_gsf` added. `-o DIR` writes it to DIR
instead. The set is:

- one `.gsflib` with the game's image and the stub, named after the cartridge's product code: `AGB-`, the game code
  from the cartridge's header, and the region that the code's last letter gives, such as `EUR`
- one `.minigsf` for each song, named after its place in the set and its title, such as
  `02 Title Screen, Circus Act 1.minigsf`. The set lists the songs by number or, for a game that `gsfrip` has titles
  for, such as Aero the Acro-Bat, in the order in which a player first hears them.
- `!playlist.m3u`, which lists the minis in order

The stub takes the place of the game's start-up code. The cartridge's first word branches to it, after the end of the
game's image. It sets up the stacks and an interrupt handler, makes the calls that the game's start-up code makes to set
up the sound driver, and starts the song whose number the mini stores. It then runs the driver as the game does: the
routines that the game's interrupt handler calls, and those that its main loop calls after each vertical blank. Each
mini holds the song's number, a word at a fixed address in the stub.

Each song then plays on a model of the console that runs the game's sound driver:

- The length comes from the song's commands: two passes of the loop, to the second, and a fade of 10 s, or the end of
  the song, to the millisecond, and a fade of 1 s for its last notes to die away. A song that plays no notes gets no
  mini.
- Coverage records every byte of the cartridge that the run reads. The run lasts for the length and fade of the song,
  half the length more for a song that loops, and a margin of 10 s.
- The library keeps only the bytes that some song read, the cartridge's header and the stub. The other bytes are
  zeros, which compress to almost nothing. With Nintendo R&D1's driver, the library also keeps the whole of each sample
  that a song read part of, and all of the driver's code. With Quintet's driver, whose sound DMA reads each sample
  from the cartridge, the library keeps the whole of each sample that a song starts. With MP2K, the library keeps all
  of the driver's code.

The model runs no interrupts, timers or sound DMA between the driver's calls, and its PSG channels never stop on
their own. On the console, those can come in another order and make the driver read bytes that the model's runs
don't. That's why the library keeps whole samples and whole stretches of code for the drivers above. `--no-trim` keeps
the whole image if a trimmed set ever plays differently from an untrimmed one.

The model doesn't run the DMA transfers at the blanks or for the sound FIFOs either. `gsfrip` counts those that read
the cartridge and reports them. The rip fails if the trim would leave out a byte that one of them reads in its first
transfer, since the set would then play wrongly. What such a transfer reads after that depends on the order of the
console's interrupts, so the rip warns that the set may play differently from an untrimmed one. With Quintet's driver,
whose sound DMA reads each sample from the cartridge, the library keeps the whole of each sample, and the rip doesn't
warn.

Each mini has the tags `title`, `game`, `length`, `fade` and `utf8`, and `artist`, `year`, `copyright`, `comment`,
`gsfby` and `tagger` where they're known or given. The library has no tags. For Aero the Acro-Bat: Rascal Rival Revenge,
`gsfrip` knows the game's name, the composer, the arranger, the developer and publisher, the date of the first release,
and a title for each song that names where the game plays it, such as "Title Screen, Circus Act 1" or "Game Over".
The minis are numbered in the order in which a player first hears the songs in the game, so the title screen's song
is `02`. The two songs that the game never plays come last, with "Unused" in their titles. The game over's commands go
on in silence after its last sound. Its length is the end of that sound, with a fade of 1 s. For Hugo: The Evil Mirror,
`gsfrip` knows the game's name, the composers, the developer and publisher, the date of the first release and the
songs' titles, and the minis follow the same order. Each of its songs that ends has the end of its last sound as its
length, with a fade of 1 s. The options below replace any of the tags.

Options:

- `-o DIR`, `--output DIR`: write the set to DIR
- `--list`: list the songs
- `--only REGEX`: rip only the songs whose names or titles match, such as `"^Song 12$"`
- `--no-length`: skip the length analysis
- `--no-trim`: keep the whole image in the library
- `--margin S`: the seconds of play after the fade that the coverage still covers (default 10)
- `--jobs N`: the number of songs that run at once (default: the number of CPU threads)
- `--game`, `--artist`, `--year`, `--copyright`: the tags of those names. For a game that `gsfrip` knows, they default
  to its tags. Otherwise `--game` defaults to the image's file name. `--year` takes the date of the game's first
  release, YYYY-MM-DD.
- `--by NAME`, `--tagger NAME`: the `gsfby` and `tagger` tags, written only when given

## Building

You need Git, CMake 3.20 or later, and C and C++20 compilers. gsf builds on four libraries, which the repository
includes as submodules in `externals/`: xsflib for the PSF container and the frame of the rip tool, agblib for the Game
Boy Advance's code and the sound drivers, ntdlib for the ARM interpreter that agblib uses, and seqlib, which agblib's
work on songs requires. Clone with
`git clone --recursive`, or run `git submodule update --init` in a clone. zlib is downloaded and built along with gsf
when it isn't installed.

- **Windows:** Visual Studio 2022 or later with the "Desktop development with C++" workload and its "C++ Clang tools for
  Windows" component. The workload includes CMake. The component adds clang-cl. Configure with
  `cmake -S . -B build -A x64 -T ClangCL` instead of the configure command below. Run the commands in a Developer
  PowerShell or Developer Command Prompt.
- **macOS on Apple Silicon:** the Xcode command line tools (`xcode-select --install`) and CMake (`brew install cmake`).

Linux, Intel Macs, MSVC and 32-bit Windows aren't supported.

```sh
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release
```

The build makes `gsfrip` in `build`, or in `build/Release` on Windows.

`CMakePresets.json` has `release` and `debug` presets for the same steps: `cmake --preset release`,
`cmake --build --preset release` and `ctest --preset release`. The presets require CMake 3.23 or later. They don't
select clang-cl. On Windows, use the commands above.

## Known gaps

- The length of a song comes from its commands, not from the sound. A song that ends gets a second after its last
  command for its notes to die away.
- A song plays as its commands give it. What a game's code changes while a song plays, such as a fade or a track
  that it mutes, stays out.
- Players can differ in level. Aero the Acro-Bat switches the sound off and on after it sets up its driver, and the
  stub does the same. mGBA 0.11 then plays the Direct Sound channels at half volume, and some other players don't. The
  sets play at the level that the game itself plays at in each player.

## Layout

```
src/gsf/             GSF's files and the loading of a set
src/rip/             the ripper: the ARM assembler, the stub, the model's runs and the drivers
src/tools/           gsfrip
externals/           xsflib, agblib, ntdlib and seqlib
tests/               the CTest suites
```

## Credits

GSF was developed by CaitSith2 and Zoopd.

## Licence

gsf is under the MIT licence (`LICENSE`). `THIRD-PARTY-NOTICES.md` lists the libraries that gsf uses, and what a binary
distribution must include. This licence doesn't cover game data. Rips contain the code and sound data of a game and are
for personal use.
