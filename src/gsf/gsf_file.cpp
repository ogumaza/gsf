// SPDX-License-Identifier: MIT

#include "gsf/gsf_file.h"

#include <cstdint>
#include <vector>

#include "xsflib/psf/psf.h"

namespace gsf
{

std::vector<uint8_t> SectionProgram(const Section& section)
{
    std::vector<uint8_t> program;
    program.reserve(kSectionHeaderSize + section.data.size());
    xsflib::psf::Put32(program, section.entry);
    xsflib::psf::Put32(program, section.address);
    xsflib::psf::Put32(program, uint32_t(section.data.size()));
    program.insert(program.end(), section.data.begin(), section.data.end());

    return program;
}

std::vector<uint8_t> WriteGsf(const Section& section, const xsflib::psf::Tags& tags)
{
    return xsflib::psf::WritePsf(kGsfVersion, {}, SectionProgram(section), tags);
}

} // namespace gsf
