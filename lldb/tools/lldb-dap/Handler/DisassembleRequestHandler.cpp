//===-- DisassembleRequestHandler.cpp -------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "DAP.h"
#include "EventHelper.h"
#include "JSONUtils.h"
#include "LLDBUtils.h"
#include "Protocol/ProtocolRequests.h"
#include "Protocol/ProtocolTypes.h"
#include "ProtocolUtils.h"
#include "RequestHandler.h"
#include "lldb/API/SBAddress.h"
#include "lldb/API/SBInstruction.h"
#include "lldb/API/SBLineEntry.h"
#include "lldb/API/SBTarget.h"
#include "lldb/lldb-types.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Error.h"
#include <cstdint>
#include <optional>

using namespace lldb_dap::protocol;

namespace lldb_dap {

static protocol::DisassembledInstruction GetInvalidInstruction() {
  DisassembledInstruction invalid_inst;
  invalid_inst.address = LLDB_INVALID_ADDRESS;
  invalid_inst.presentationHint =
      DisassembledInstruction::eDisassembledInstructionPresentationHintInvalid;
  return invalid_inst;
}

static lldb::SBAddress GetDisassembleStartAddress(lldb::SBTarget target,
                                                  lldb::SBAddress addr,
                                                  int64_t instruction_offset) {
  if (instruction_offset == 0)
    return addr;

  if (target.GetMinimumOpcodeByteSize() == target.GetMaximumOpcodeByteSize()) {
    // We have fixed opcode size, so we can calculate the address directly,
    // negative or positive.
    lldb::addr_t load_addr = addr.GetLoadAddress(target);
    load_addr += instruction_offset * target.GetMinimumOpcodeByteSize();
    return lldb::SBAddress(load_addr, target);
  }

  if (instruction_offset > 0) {
    lldb::SBInstructionList forward_insts =
        target.ReadInstructions(addr, instruction_offset + 1);
    return forward_insts.GetInstructionAtIndex(forward_insts.GetSize() - 1)
        .GetAddress();
  }

  // We have a negative instruction offset, so we need to disassemble backwards.
  // The opcode size is not fixed, so we have no idea where to start from.
  // Let's try from the start of the current symbol if available.
  auto symbol = addr.GetSymbol();
  if (!symbol.IsValid())
    return addr;

  // Add valid instructions before the current instruction using the symbol.
  lldb::SBInstructionList symbol_insts =
      target.ReadInstructions(symbol.GetStartAddress(), addr, nullptr);
  if (!symbol_insts.IsValid() || symbol_insts.GetSize() == 0)
    return addr;

  const auto backwards_instructions_count =
      static_cast<size_t>(std::abs(instruction_offset));
  if (symbol_insts.GetSize() < backwards_instructions_count) {
    // We don't have enough instructions to disassemble backwards, so just
    // return the start address of the symbol.
    return symbol_insts.GetInstructionAtIndex(0).GetAddress();
  }

  return symbol_insts
      .GetInstructionAtIndex(symbol_insts.GetSize() -
                             backwards_instructions_count)
      .GetAddress();
}

static DisassembledInstruction ConvertSBInstructionToDisassembledInstruction(
    DAP &dap, lldb::SBInstruction &inst, bool resolve_symbols) {
  lldb::SBTarget target = dap.target;
  if (!inst.IsValid())
    return GetInvalidInstruction();

  auto addr = inst.GetAddress();
  const auto inst_addr = addr.GetLoadAddress(target);

  // FIXME: This is a workaround - this address might come from
  // disassembly that started in a different section, and thus
  // comparisons between this object and other address objects with the
  // same load address will return false.
  addr = lldb::SBAddress(inst_addr, target);

  const char *m = inst.GetMnemonic(target);
  const char *o = inst.GetOperands(target);
  std::string c = inst.GetComment(target);
  auto d = inst.GetData(target);

  std::string bytes;
  llvm::raw_string_ostream sb(bytes);
  for (unsigned i = 0; i < inst.GetByteSize(); i++) {
    lldb::SBError error;
    uint8_t b = d.GetUnsignedInt8(error, i);
    if (error.Success())
      sb << llvm::format("%2.2x ", b);
  }

  DisassembledInstruction disassembled_inst;
  disassembled_inst.address = inst_addr;

  if (!bytes.empty()) // remove last whitespace
    bytes.pop_back();
  disassembled_inst.instructionBytes = std::move(bytes);

  llvm::raw_string_ostream si(disassembled_inst.instruction);
  si << llvm::formatv("{0,-7} {1,-25}", m, o);

  // Only add the symbol on the first line of the function.
  // in the comment section
  if (lldb::SBSymbol symbol = addr.GetSymbol();
      symbol.GetStartAddress() == addr) {
    const llvm::StringRef sym_display_name = symbol.GetDisplayName();
    c.append(" ");
    c.append(sym_display_name);

    if (resolve_symbols)
      disassembled_inst.symbol = sym_display_name;
  }

  if (!c.empty()) {
    si << " ; " << c;
  }

  std::optional<protocol::Source> source = dap.ResolveSource(addr);
  lldb::SBLineEntry line_entry = GetLineEntryForAddress(target, addr);

  // If the line number is 0 then the entry represents a compiler generated
  // location.
  if (source && !IsAssemblySource(*source) &&
      line_entry.GetStartAddress() == addr && line_entry.IsValid() &&
      line_entry.GetFileSpec().IsValid() && line_entry.GetLine() != 0) {

    disassembled_inst.location = std::move(source);
    const auto line = line_entry.GetLine();
    if (line != 0 && line != LLDB_INVALID_LINE_NUMBER)
      disassembled_inst.line = line;

    const auto column = line_entry.GetColumn();
    if (column != 0 && column != LLDB_INVALID_COLUMN_NUMBER)
      disassembled_inst.column = column;

    lldb::SBAddress end_addr = line_entry.GetEndAddress();
    auto end_line_entry = GetLineEntryForAddress(target, end_addr);
    if (end_line_entry.IsValid() &&
        end_line_entry.GetFileSpec() == line_entry.GetFileSpec()) {
      const auto end_line = end_line_entry.GetLine();
      if (end_line != 0 && end_line != LLDB_INVALID_LINE_NUMBER &&
          end_line != line) {
        disassembled_inst.endLine = end_line;

        const auto end_column = end_line_entry.GetColumn();
        if (end_column != 0 && end_column != LLDB_INVALID_COLUMN_NUMBER &&
            end_column != column)
          disassembled_inst.endColumn = end_column - 1;
      }
    }
  }

  return disassembled_inst;
}

/// Disassembles code stored at the provided location.
/// Clients should only call this request if the corresponding capability
/// `supportsDisassembleRequest` is true.
llvm::Expected<DisassembleResponseBody>
DisassembleRequestHandler::Run(const DisassembleArguments &args) const {
  std::optional<lldb::addr_t> addr_opt =
      DecodeMemoryReference(args.memoryReference);
  if (!addr_opt.has_value())
    return llvm::make_error<DAPError>("Malformed memory reference: " +
                                      args.memoryReference);

  lldb::addr_t addr_ptr = *addr_opt;
  addr_ptr += args.offset.value_or(0);
  lldb::SBAddress addr(addr_ptr, dap.target);
  if (!addr.IsValid())
    return llvm::make_error<DAPError>(
        "Memory reference not found in the current binary.");

  // Offset (in instructions) to be applied after the byte offset (if any)
  // before disassembling. Can be negative.
  int64_t instruction_offset = args.instructionOffset.value_or(0);

  // Calculate a sufficient address to start disassembling from.
  lldb::SBAddress disassemble_start_addr =
      GetDisassembleStartAddress(dap.target, addr, instruction_offset);
  if (!disassemble_start_addr.IsValid())
    return llvm::make_error<DAPError>(
        "Unexpected error while disassembling instructions.");

  lldb::SBInstructionList insts = dap.target.ReadInstructions(
      disassemble_start_addr, args.instructionCount);
  if (!insts.IsValid())
    return llvm::make_error<DAPError>(
        "Unexpected error while disassembling instructions.");

  // Conver the found instructions to the DAP format.
  const bool resolve_symbols = args.resolveSymbols.value_or(false);
  std::vector<DisassembledInstruction> instructions;
  size_t original_address_index = args.instructionCount;
  for (size_t i = 0; i < insts.GetSize(); ++i) {
    lldb::SBInstruction inst = insts.GetInstructionAtIndex(i);
    if (inst.GetAddress() == addr)
      original_address_index = i;

    instructions.push_back(ConvertSBInstructionToDisassembledInstruction(
        dap, inst, resolve_symbols));
  }

  // Check if we miss instructions at the beginning.
  if (instruction_offset < 0) {
    const auto backwards_instructions_count =
        static_cast<size_t>(std::abs(instruction_offset));
    if (original_address_index < backwards_instructions_count) {
      // We don't have enough instructions before the main address as was
      // requested. Let's pad the start of the instructions with invalid
      // instructions.
      std::vector<DisassembledInstruction> invalid_instructions(
          backwards_instructions_count - original_address_index,
          GetInvalidInstruction());
      instructions.insert(instructions.begin(), invalid_instructions.begin(),
                          invalid_instructions.end());

      // Trim excess instructions if needed.
      if (instructions.size() > args.instructionCount)
        instructions.resize(args.instructionCount);
    }
  }

  // Pad the instructions with invalid instructions if needed.
  while (instructions.size() < args.instructionCount) {
    instructions.push_back(GetInvalidInstruction());
  }

  return DisassembleResponseBody{std::move(instructions)};
}

} // namespace lldb_dap
