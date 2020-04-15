//===- SyntheticSections.cpp ---------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "SyntheticSections.h"
#include "InputFiles.h"
#include "OutputSegment.h"
#include "Symbols.h"
#include "Writer.h"

#include "lld/Common/ErrorHandler.h"
#include "llvm/Support/EndianStream.h"
#include "llvm/Support/LEB128.h"

using namespace llvm;
using namespace llvm::MachO;
using namespace llvm::support;

namespace lld {
namespace macho {

MachHeaderSection::MachHeaderSection() {
  // dyld3's MachOLoaded::getSlide() assumes that the __TEXT segment starts
  // from the beginning of the file (i.e. the header).
  segname = segment_names::text;
  name = section_names::header;
}

void MachHeaderSection::addLoadCommand(LoadCommand *lc) {
  loadCommands.push_back(lc);
  sizeOfCmds += lc->getSize();
}

size_t MachHeaderSection::getSize() const {
  return sizeof(mach_header_64) + sizeOfCmds;
}

void MachHeaderSection::writeTo(uint8_t *buf) {
  auto *hdr = reinterpret_cast<mach_header_64 *>(buf);
  hdr->magic = MH_MAGIC_64;
  hdr->cputype = CPU_TYPE_X86_64;
  hdr->cpusubtype = CPU_SUBTYPE_X86_64_ALL | CPU_SUBTYPE_LIB64;
  hdr->filetype = MH_EXECUTE;
  hdr->ncmds = loadCommands.size();
  hdr->sizeofcmds = sizeOfCmds;
  hdr->flags = MH_NOUNDEFS | MH_DYLDLINK | MH_TWOLEVEL;

  uint8_t *p = reinterpret_cast<uint8_t *>(hdr + 1);
  for (LoadCommand *lc : loadCommands) {
    lc->writeTo(p);
    p += lc->getSize();
  }
}

PageZeroSection::PageZeroSection() {
  segname = segment_names::pageZero;
  name = section_names::pageZero;
}

GotSection::GotSection() {
  segname = "__DATA_CONST";
  name = "__got";
  align = 8;
  flags = S_NON_LAZY_SYMBOL_POINTERS;

  // TODO: section_64::reserved1 should be an index into the indirect symbol
  // table, which we do not currently emit
}

void GotSection::addEntry(DylibSymbol &sym) {
  if (entries.insert(&sym)) {
    sym.gotIndex = entries.size() - 1;
  }
}

BindingSection::BindingSection() {
  segname = segment_names::linkEdit;
  name = section_names::binding;
}

bool BindingSection::isNeeded() const { return in.got->isNeeded(); }

// Emit bind opcodes, which are a stream of byte-sized opcodes that dyld
// interprets to update a record with the following fields:
//  * segment index (of the segment to write the symbol addresses to, typically
//    the __DATA_CONST segment which contains the GOT)
//  * offset within the segment, indicating the next location to write a binding
//  * symbol type
//  * symbol library ordinal (the index of its library's LC_LOAD_DYLIB command)
//  * symbol name
//  * addend
// When dyld sees BIND_OPCODE_DO_BIND, it uses the current record state to bind
// a symbol in the GOT, and increments the segment offset to point to the next
// entry. It does *not* clear the record state after doing the bind, so
// subsequent opcodes only need to encode the differences between bindings.
void BindingSection::finalizeContents() {
  if (!isNeeded())
    return;

  raw_svector_ostream os{contents};
  os << static_cast<uint8_t>(BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB |
                             in.got->parent->index);
  encodeULEB128(in.got->addr - in.got->parent->firstSection()->addr, os);
  for (const DylibSymbol *sym : in.got->getEntries()) {
    // TODO: Implement compact encoding -- we only need to encode the
    // differences between consecutive symbol entries.
    if (sym->file->ordinal <= BIND_IMMEDIATE_MASK) {
      os << static_cast<uint8_t>(BIND_OPCODE_SET_DYLIB_ORDINAL_IMM |
                                 sym->file->ordinal);
    } else {
      error("TODO: Support larger dylib symbol ordinals");
      continue;
    }
    os << static_cast<uint8_t>(BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM)
       << sym->getName() << '\0'
       << static_cast<uint8_t>(BIND_OPCODE_SET_TYPE_IMM | BIND_TYPE_POINTER)
       << static_cast<uint8_t>(BIND_OPCODE_DO_BIND);
  }

  os << static_cast<uint8_t>(BIND_OPCODE_DONE);
}

void BindingSection::writeTo(uint8_t *buf) {
  memcpy(buf, contents.data(), contents.size());
}

InStruct in;

} // namespace macho
} // namespace lld