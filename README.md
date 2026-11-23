# gsf

gsf rips the music of Game Boy Advance games to GSF, the PSF-family format for Game Boy Advance music. A GSF set
contains the game's cartridge image, cut down to the code and data that its music reads, and a small stub that starts
one song. A player emulates the console and runs the game's sound driver, so the music plays as it does in the game.

`gsfrip` finds a game's sound driver, writes the stub for it and rips the set. gsf has no player of its own. The GSF
players built on mGBA play the sets.

## Supported drivers

`gsfrip` rips games with these sound drivers:

- AlphaDream's driver: Mario & Luigi: Superstar Saga and two Hamtaro games (Tottoko Hamtaro 4: Nijiiro Daikoushin
  Dechu and Tottoko Hamtaro: Hamu-Hamu Sports)
- Brownie Brown's driver: Sword of Mana, the US release, and Magical Vacation. Each sound that uses the music's
  channels is a song. With `--effects`, Sword of Mana's other sounds, its sound effects, are ripped too. Magical
  Vacation's sound effects aren't ripped.
- Frontline Studios' MOD player, the older player of the F U H Sound System: Hardcore Pool, the European release, and
  Garfield: The Search for Pooky and Games Explosion!, the US releases, and revision 2 of Garfield's European release.
  Each part of a module that the game starts at an order is a song, with the fade-in that the game starts it with. The
  parts that no start of the game reaches follow as songs too, titled "(Unused)": three in Games Explosion!.
  Garfield's songs are numbered through its two modules. The sound effects play samples without a
  sequence and aren't ripped.
- Frontline Studios' XM-like player, the newer player of the F U H Sound System: Chicken Shoot, the European release,
  and Chicken Shoot 2, the US release. The title's music and each order list of the game's levels are songs, five in
  each game, at the master volumes that the games set on a new save. The sound effects aren't ripped.
- ITE Media's driver: Hugo: The Evil Mirror, the US release. Each song of the game's bank is a song, and it plays at
  the music volume that the game sets for it. A song that ends plays out to its last sound. In the game, the
  interrupted music comes back after a jingle's last row and cuts the jingle short. The audio of the game's videos
  isn't ripped.
- Konami's driver: Yu-Gi-Oh! Duel Monsters Expert 2006, the Japanese release; Yu-Gi-Oh! World Championship
  Tournament 2004, the European release; Yu-Gi-Oh! 7 Trials to Glory: World Championship Tournament 2005, Yu-Gi-Oh!
  GX: Duel Academy and Rave Master: Special Attack Force, the US releases; Hunter X Hunter: Minna Tomodachi
  Daisakusen!!; Hikaru no Go 2; Tennis no Ouji-sama 2003: Passion Red, Tennis no Ouji-sama: Aim at the Victory! and
  Tennis no Ouji-sama 2004: Glorious Gold and Stylish Silver; and Mermaid Melody Pichi Pichi Pitch and Mermaid Melody
  Pichi Pichitto Live Start!. Each song of the song table is a song. The Mermaid Melody games play in mono, as they
  start with no save.
- Logik State's driver, GBAModPlay: Aero the Acro-Bat: Rascal Rival Revenge, the European and US releases; Zapper, the
  US release; and Pinball Challenge Deluxe, the European release. In Aero and Zapper, each of the game's modules is a
  song, and the songs are numbered in the order of the modules in the image. Aero's set adds the part of one module that
  the game skips. Pinball Challenge Deluxe starts its pieces at orders within its modules, and each piece is a song. A
  jingle ends where the game would go back to the music that it interrupted. The songs play at the music's volume that
  the game starts with.
- Nintendo R&D1's driver: WarioWare, Inc.: Mega Microgame$! and Rhythm Tengoku. Each entry of the song table that
  contains a song is a song, sound effects included. A song plays as the song table starts it. The games' stages can
  play it at another tempo, pitch or volume, or with some tracks silent, and the sets don't follow them.
- Nintendo R&D2's driver: Super Mario Advance 2, 3 and 4 and The Legend of Zelda: A Link to the Past & Four Swords, the
  US releases. Each sequence of the music is a song. A sequence that the game plays by a music number plays as the first
  such number sets it up: with its voices, its volume and its muted tracks. In Super Mario Advance 2, the tracks that
  play only while Mario rides Yoshi stay silent. With `--effects`, A Link to the Past's sound effects are ripped too,
  each on the player that the game plays it on. The game never plays the second sequence of sound effects, or three
  tracks of the first, so their titles say "Unused" and they come last. A few of the unused ones play samples from
  outside the cartridge, which no set can contain, and aren't ripped. The Super Mario Advance games' sound effects
  aren't ripped.
- Quintet's driver: Super Robot Taisen A, D, R and J; Bouken Yuuki Plaston GP, Bouken Yuuki Densetsu Plaston Gate and
  Bouken Yuuki PlaStar World: Densetsu no Plust Gate EX; Legend of Dynamic: Goushouden; Blender Bros.; Doubutsu no
  Shima no Chobi Gurumi; Gachasute! Dyna Device Red and Blue; Robopon 2: Ring Version and Cross Version; and Magical
  Fengshen. The songs of each music file are songs, numbered one file after another. With `--effects`, the sound
  effects of the four Super Robot Taisen games are ripped too, started through the game's routine for them. The other
  games' sound effects aren't ripped.
- Webfoot Technologies' driver: Dragon Ball Z: The Legacy of Goku, The Legacy of Goku II and Buu's Fury, and the
  cartridge with both Dragon Ball Z: Buu's Fury and Dragon Ball GT: Transformation, the US releases. Each song of a song
  table is a song, and it plays at the music volume that the game starts with: 4 of 8, and 3 in GT Transformation. GT
  Transformation halves its music volume on one screen, and the set doesn't follow it. The sound effects play samples
  without a sequence and aren't ripped.
- MP2K, the sound driver of Nintendo's SDK, which most GBA games use. Each entry of the song table is a song, and it
  plays as the driver starts it. Where a game's code sets a song up before it starts it, with another number of
  channels, a reverb or a volume, the set does the same. Golden Sun: The Lost Age has Camelot's version of the driver
  and plays each song through Camelot's sound module, as the game does. gsf doesn't rip the other games with Camelot's
  version, such as Mario Tennis: Power Tour. `gsfrip` names the reason.

The Legend of Zelda: A Link to the Past & Four Swords has two drivers: Nintendo R&D2's plays A Link to the Past, and
MP2K plays the menus and Four Swords. `gsfrip` rips a set for each. The cartridge with Buu's Fury and GT Transformation
contains three programs, the menu and the two games, each with a copy of Webfoot Technologies' driver and a song table.
`gsfrip` rips a set for each: `Menu`, `Buu's Fury` and `GT Transformation`. Their songs are numbered through the three
song tables, as `--list` shows: the menu has song 0, Buu's Fury songs 1 to 53 and GT Transformation songs 54 to 78.

A game with another driver gets "no sound driver that gsf rips found".

## Ripping a game

```
gsfrip game.gba
```

`gsfrip` writes the set to a folder next to the image, named after it with `_gsf` added. `-o DIR` writes it to DIR
instead. The set is:

- one `.gsflib` with the game's image and the stub, named after the cartridge's product code: `AGB-`, the game code
  from the cartridge's header, and the region that the code's last letter gives, such as `EUR`
- one `.minigsf` for each song, named after its place in the set and its number, such as `02 Song 1.minigsf`. The set
  lists the songs by number.
- `!playlist.m3u`, which lists the minis in order

With `--effects`, the set contains the game's sound effects too, for the games above whose sound effects `gsfrip` can
start. They follow the music, as `Effect N` with the number that `--list` gives, and the stub starts each one as the
game's code does. A sound effect plays alone, with no music under it. Most play once and end, so their length is their
end and a fade of 1 s. The few that loop get two passes and a fade of 7 s. In the game, the code stops them.

A game with a driver for each part, such as A Link to the Past & Four Swords, gets a folder for each part's set inside
that folder, named after the part: `A Link to the Past` and `Four Swords`. Each contains a set as above, with a library
of its own.

The stub takes the place of the game's start-up code. The cartridge's first word branches to it. It goes after the end
of the game's image. In an image that fills the cartridge's 32 MiB, it goes in the padding at the end, or else in the
image's longest run of zeros or of 0xFF bytes. It sets up the stacks and an interrupt handler, makes the calls that the
game's start-up code makes to set up the sound driver, and starts the song whose number the mini stores. It then runs
the driver as the game does: the routines that the game's interrupt handler calls, and those that its main loop calls
after each vertical blank. With Webfoot Technologies' driver, the stub installs the game's interrupt handler instead of
writing one: the driver's handler returns through registers that only the game's handler sets, and lets the next
interrupt in while it mixes. Each mini contains the song's number: two bytes at 0x080000BE, in the reserved bytes at the
end of the cartridge's header.

The length of each song comes from its commands, as agblib's conversions read them: two passes of the loop, to the
millisecond, and a fade of 7 s, or the end of the song, to the millisecond, and a fade of 1 s for its last notes to die
away. A song that makes no sound, with no notes or only notes at a level of 0, gets no mini.

Each song then plays on a model of the console that runs the game's sound driver. The run records every byte of the
cartridge that the song reads. It lasts for the length and fade of the song, half the length more for a song that
loops, and a margin of 10 s. The library keeps only the bytes that some song read, the cartridge's header and the stub.
The other bytes are zeros, which compress to almost nothing. Some drivers keep more:

- Nintendo R&D1's driver: the whole of each sample, instrument record and sample header that a song read part of, and
  all of the driver's code.
- Quintet's driver, whose sound DMA reads each sample from the cartridge: the whole of each sample that a song starts.
- MP2K: all of the driver's code.
- Brownie Brown's and Konami's drivers: the whole of each sample that a song read part of, and all of the driver's code.
- Webfoot Technologies' driver: the whole of each sample that a song read part of, with its entry in the sample table,
  the driver's code and tables, and the game's interrupt handler.
- Frontline Studios' MOD player: the whole of each sample that a song read part of, with its instrument's records, and
  the whole table of periods.
- Frontline Studios' XM-like player: the whole of each instrument that a song read part of, the player's code and its
  tables of periods, steps and waves.

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

Each mini has the tags `title`, `game`, `length`, `fade` and `utf8`, and `artist`, `year`, `copyright`, `gsfby` and
`tagger` where they're given. A song that loops also has `loop_start`, the time to the start of its loop, and
`loop_length`, the time of one pass. `length` is `loop_start` plus two passes, or a little less for a loop with no intro
whose first pass is a little shorter. A player that reads the two tags can play another number of passes. Other players
show them as metadata and play the file as `length` gives it. The title is the song's number, such as "Song 5" or
"Effect 12", with "(Unused)" after it where the game never plays it. The library has no tags. In Aero the Acro-Bat:
Rascal Rival Revenge, the game over's commands go on in silence after its last sound. Its length is the end of that
sound, with a fade of 1 s. Each song of Hugo: The Evil Mirror that ends has the end of its last sound as its length,
with a fade of 1 s. The options below set the other tags.

Options:

- `-o DIR`, `--output DIR`: write the set to DIR
- `--list`: list the songs of each set
- `--only REGEX`: rip only the songs whose names or titles match, such as `"^Song 12$"`, in each set. A sound effect
  matches as `Effect N` too
- `--effects`: rip the sound effects too, where `gsfrip` can start them
- `--bgm`: rip only the music (the default)
- `--no-length`: skip the length analysis
- `--no-trim`: keep the whole image in the library
- `--margin S`: the seconds of play after the fade that the coverage still covers (default 10)
- `--jobs N`: the number of songs that run at once (default: the number of CPU threads)
- `--game`, `--artist`, `--year`, `--copyright`: the tags of those names. `--game` defaults to the image's file name.
  `--year` takes the date of the game's first release, YYYY-MM-DD.
- `--by NAME`, `--tagger NAME`: the `gsfby` and `tagger` tags, written only when given

## Building

You need Git, CMake 3.20 or later, and C and C++20 compilers. gsf builds on four libraries, which the repository
includes as submodules in `externals/`: xsflib for the PSF container and the frame of the rip tool, agblib for the Game
Boy Advance's code and the sound drivers, ntdlib for the ARM interpreter that agblib uses and the ARM assembler that
writes the stub, and seqlib for the names of the files and agblib's work on songs. Clone with `git clone --recursive`, or run
`git submodule update --init` in a clone. zlib is downloaded and built along with gsf when it isn't installed.

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
  command for its notes to die away. Where a note rings on for longer, the fade cuts it.
- The length tags count from the song's start, not from its first sound. A player that skips the silence at the start
  of a file plays a song's loop or end that much later.
- A song plays as its commands give it. What a game's code changes while a song plays, such as a fade or a track
  that it mutes, stays out.
- Aero the Acro-Bat's sets play at different levels in different players. The game switches the sound off and on
  after it sets up its driver, and the stub does the same. mGBA 0.11 then plays the Direct Sound channels at half
  volume. foo_input_gsf 3.0.23 doesn't halve them and plays these sets louder. In each player, a set plays at the
  level that the game itself plays at in that player.

## Layout

```
src/gsf/             GSF's files and the loading of a set
src/rip/             the ripper: the stub, the model's runs and the drivers
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
