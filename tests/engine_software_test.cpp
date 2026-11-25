// SPDX-License-Identifier: MIT

// Tests of the stub's operations for the steps of the game's code that agblib gives for Engine Software's driver: the
// set-up's calls and stores, and the songs' starts merged into calls from tables by song.

#include "rip/drivers/engine_software.h"

#include <cstdint>
#include <stdexcept>
#include <vector>

#include "agblib/engine_software/driver.h"
#include "xsflib/testing/check.h"

#include "rip/stub.h"

namespace
{

using agblib::engine_software::GameStep;
using gsf::rip::Operation;

constexpr uint32_t kStart = 0x08000101;
constexpr uint32_t kVolume = 0x08000201;
constexpr uint32_t kJingle = 0x08000301;
constexpr uint32_t kObject = 0x02000000;

// A store and a call keep their kinds, addresses and values.
void TestStepOperations()
{
    const std::vector<GameStep> steps = {{.address = kObject, .values = {4}, .size = 4},
                                         {.address = kStart, .values = {kObject, 9}}};

    const std::vector<Operation> operations = gsf::rip::StepOperations(steps);

    XSFLIB_CHECK(operations.size() == 2);
    XSFLIB_CHECK(operations[0].kind == Operation::Kind::kStore && operations[0].address == kObject &&
                 operations[0].size == 4 && operations[0].values.at(0).constant == 4);
    XSFLIB_CHECK(operations[1].kind == Operation::Kind::kCall && operations[1].address == kStart &&
                 operations[1].routines.empty() && operations[1].values.size() == 2 &&
                 operations[1].values[1].constant == 9);
}

// Three songs: a start, a jingle of two calls and another start. The first place calls each song's first routine with
// its arguments from tables. The second calls only the jingle's routine, and the other songs skip it.
void TestSongStartOperations()
{
    const std::vector<std::vector<GameStep>> starts = {
        {{.address = kStart, .values = {kObject, 0, 0x40, 0xD2}}},
        {{.address = kVolume, .values = {0xD2}}, {.address = kJingle, .values = {kObject, 1}}},
        {{.address = kStart, .values = {kObject, 2, 1, 0xD2}}},
    };

    const std::vector<Operation> operations = gsf::rip::SongStartOperations(starts);

    XSFLIB_CHECK(operations.size() == 2);
    XSFLIB_CHECK(operations[0].routines == (std::vector<uint32_t>{kStart, kVolume, kStart}));
    XSFLIB_CHECK(operations[0].values.size() == 4);
    XSFLIB_CHECK(operations[0].values[0].table == (std::vector<uint32_t>{kObject, 0xD2, kObject}));
    XSFLIB_CHECK(operations[0].values[2].table == (std::vector<uint32_t>{0x40, 0, 1}));
    XSFLIB_CHECK(operations[1].routines == (std::vector<uint32_t>{0, kJingle, 0}));
    XSFLIB_CHECK(operations[1].values.size() == 2);
    XSFLIB_CHECK(operations[1].values[1].table == (std::vector<uint32_t>{0, 1, 0}));
}

// A song's start is calls only.
void TestStartWithStore()
{
    const std::vector<std::vector<GameStep>> starts = {{{.address = kObject, .values = {1}, .size = 1}}};

    bool threw = false;
    try
    {
        gsf::rip::SongStartOperations(starts);
    }
    catch (const std::logic_error&)
    {
        threw = true;
    }

    XSFLIB_CHECK(threw);
}

} // namespace

int main()
{
    TestStepOperations();
    TestSongStartOperations();
    TestStartWithStore();

    return xsflib::testing::Summary("engine_software");
}
