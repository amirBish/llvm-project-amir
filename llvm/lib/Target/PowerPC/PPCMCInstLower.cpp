//===-- PPCMCInstLower.cpp - Convert PPC MachineInstr to an MCInst --------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file contains code to lower PPC MachineInstrs to their corresponding
// MCInst records.
//
//===----------------------------------------------------------------------===//

#include "MCTargetDesc/PPCMCAsmInfo.h"
#include "PPC.h"
#include "PPCMachineFunctionInfo.h"
#include "PPCSubtarget.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/Twine.h"
#include "llvm/CodeGen/AsmPrinter.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineModuleInfoImpls.h"
#include "llvm/CodeGen/TargetLowering.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/GlobalValue.h"
#include "llvm/IR/Mangler.h"
#include "llvm/IR/Module.h"
#include "llvm/MC/MCAsmInfo.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/MC/MCInst.h"
#include "llvm/Target/TargetLoweringObjectFile.h"
using namespace llvm;

static MCSymbol *GetSymbolFromOperand(const MachineOperand &MO,
                                      AsmPrinter &AP) {
  if (MO.isGlobal()) {
    // Get the symbol from the global, accounting for XCOFF-specific
    // intricacies (see TargetLoweringObjectFileXCOFF::getTargetSymbol).
    const GlobalValue *GV = MO.getGlobal();
    return AP.getSymbol(GV);
  }

  assert(MO.isSymbol() && "Isn't a symbol reference");

  SmallString<128> Name;
  const DataLayout &DL = AP.getDataLayout();
  Mangler::getNameWithPrefix(Name, MO.getSymbolName(), DL);

  MCContext &Ctx = AP.OutContext;
  MCSymbol *Sym = Ctx.getOrCreateSymbol(Name);
  return Sym;
}

static MCOperand GetSymbolRef(const MachineOperand &MO, const MCSymbol *Symbol,
                              AsmPrinter &Printer) {
  MCContext &Ctx = Printer.OutContext;
  PPCMCExpr::Specifier RefKind = PPC::S_None;

  unsigned access = MO.getTargetFlags();

  switch (access) {
    case PPCII::MO_TPREL_LO:
      RefKind = PPC::S_TPREL_LO;
      break;
    case PPCII::MO_TPREL_HA:
      RefKind = PPC::S_TPREL_HA;
      break;
    case PPCII::MO_DTPREL_LO:
      RefKind = PPC::S_DTPREL_LO;
      break;
    case PPCII::MO_TLSLD_LO:
      RefKind = PPC::S_GOT_TLSLD_LO;
      break;
    case PPCII::MO_TOC_LO:
      RefKind = PPC::S_TOC_LO;
      break;
    case PPCII::MO_TLS:
      RefKind = PPC::S_TLS;
      break;
    case PPCII::MO_TLS_PCREL_FLAG:
      RefKind = PPC::S_TLS_PCREL;
      break;
  }

  const TargetMachine &TM = Printer.TM;
  const MachineInstr *MI = MO.getParent();
  const MachineFunction *MF = MI->getMF();

  if (MO.getTargetFlags() == PPCII::MO_PLT)
    RefKind = PPC::S_PLT;
  else if (MO.getTargetFlags() == PPCII::MO_PCREL_FLAG)
    RefKind = PPC::S_PCREL;
  else if (MO.getTargetFlags() == PPCII::MO_GOT_PCREL_FLAG)
    RefKind = PPC::S_GOT_PCREL;
  else if (MO.getTargetFlags() == PPCII::MO_TPREL_PCREL_FLAG)
    RefKind = PPC::S_TPREL;
  else if (MO.getTargetFlags() == PPCII::MO_GOT_TLSGD_PCREL_FLAG)
    RefKind = PPC::S_GOT_TLSGD_PCREL;
  else if (MO.getTargetFlags() == PPCII::MO_GOT_TLSLD_PCREL_FLAG)
    RefKind = PPC::S_GOT_TLSLD_PCREL;
  else if (MO.getTargetFlags() == PPCII::MO_GOT_TPREL_PCREL_FLAG)
    RefKind = PPC::S_GOT_TPREL_PCREL;
  else if (MO.getTargetFlags() == PPCII::MO_TPREL_FLAG ||
           MO.getTargetFlags() == PPCII::MO_TLSLD_FLAG) {
    assert(MO.isGlobal() && "Only expecting a global MachineOperand here!");
    TLSModel::Model Model = TM.getTLSModel(MO.getGlobal());
    const PPCFunctionInfo *FuncInfo = MF->getInfo<PPCFunctionInfo>();
    // For the local-[exec|dynamic] TLS model, we may generate the offset from
    // the TLS base as an immediate operand (instead of using a TOC entry). Set
    // the relocation type in case the result is used for purposes other than a
    // TOC reference. In TOC reference cases, this result is discarded.
    if (Model == TLSModel::LocalExec)
      RefKind = PPC::S_AIX_TLSLE;
    else if (Model == TLSModel::LocalDynamic &&
             FuncInfo->isAIXFuncUseTLSIEForLD())
      // On AIX, TLS model opt may have turned local-dynamic accesses into
      // initial-exec accesses.
      RefKind = PPC::S_AIX_TLSIE;
    else if (Model == TLSModel::LocalDynamic)
      RefKind = PPC::S_AIX_TLSLD;
  }

  const Module *M = MF->getFunction().getParent();
  const PPCSubtarget *Subtarget = &(MF->getSubtarget<PPCSubtarget>());

  unsigned MIOpcode = MI->getOpcode();
  assert((Subtarget->isUsingPCRelativeCalls() || MIOpcode != PPC::BL8_NOTOC) &&
         "BL8_NOTOC is only valid when using PC Relative Calls.");
  if (Subtarget->isUsingPCRelativeCalls()) {
    if (MIOpcode == PPC::TAILB || MIOpcode == PPC::TAILB8 ||
        MIOpcode == PPC::TCRETURNdi || MIOpcode == PPC::TCRETURNdi8 ||
        MIOpcode == PPC::BL8_NOTOC || MIOpcode == PPC::BL8_NOTOC_RM) {
      RefKind = PPC::S_NOTOC;
    }
    if (MO.getTargetFlags() == PPCII::MO_PCREL_OPT_FLAG)
      RefKind = PPC::S_PCREL_OPT;
  }

  const MCExpr *Expr = MCSymbolRefExpr::create(Symbol, RefKind, Ctx);
  // If -msecure-plt -fPIC, add 32768 to symbol.
  if (Subtarget->isSecurePlt() && TM.isPositionIndependent() &&
      M->getPICLevel() == PICLevel::BigPIC &&
      MO.getTargetFlags() == PPCII::MO_PLT)
    Expr =
        MCBinaryExpr::createAdd(Expr, MCConstantExpr::create(32768, Ctx), Ctx);

  if (!MO.isJTI() && MO.getOffset())
    Expr = MCBinaryExpr::createAdd(Expr,
                                   MCConstantExpr::create(MO.getOffset(), Ctx),
                                   Ctx);

  // Subtract off the PIC base if required.
  if (MO.getTargetFlags() == PPCII::MO_PIC_FLAG ||
      MO.getTargetFlags() == PPCII::MO_PIC_HA_FLAG ||
      MO.getTargetFlags() == PPCII::MO_PIC_LO_FLAG) {
    const MachineFunction *MF = MO.getParent()->getParent()->getParent();

    const MCExpr *PB = MCSymbolRefExpr::create(MF->getPICBaseSymbol(), Ctx);
    Expr = MCBinaryExpr::createSub(Expr, PB, Ctx);
  }

  // Add ha16() / lo16() markers if required.
  switch (access) {
    case PPCII::MO_LO:
    case PPCII::MO_PIC_LO_FLAG:
      Expr = MCSpecifierExpr::create(Expr, PPC::S_LO, Ctx);
      break;
    case PPCII::MO_HA:
    case PPCII::MO_PIC_HA_FLAG:
      Expr = MCSpecifierExpr::create(Expr, PPC::S_HA, Ctx);
      break;
  }

  return MCOperand::createExpr(Expr);
}

void llvm::LowerPPCMachineInstrToMCInst(const MachineInstr *MI, MCInst &OutMI,
                                        AsmPrinter &AP) {
  OutMI.setOpcode(MI->getOpcode());

  for (const MachineOperand &MO : MI->operands()) {
    MCOperand MCOp;
    if (LowerPPCMachineOperandToMCOperand(MO, MCOp, AP))
      OutMI.addOperand(MCOp);
  }
}

bool llvm::LowerPPCMachineOperandToMCOperand(const MachineOperand &MO,
                                             MCOperand &OutMO, AsmPrinter &AP) {
  switch (MO.getType()) {
  default:
    llvm_unreachable("unknown operand type");
  case MachineOperand::MO_Register:
    assert(!MO.getSubReg() && "Subregs should be eliminated!");
    assert(MO.getReg() > PPC::NoRegister &&
           MO.getReg() < PPC::NUM_TARGET_REGS &&
           "Invalid register for this target!");
    // ISA instructions refer to the containing dmr reg.
    if (PPC::isDMRROWpRegister(MO.getReg())) {
      OutMO =
          MCOperand::createReg(PPC::DMR0 + (MO.getReg() - PPC::DMRROWp0) / 4);
      return true;
    }
    // Ignore all implicit register operands.
    if (MO.isImplicit())
      return false;
    OutMO = MCOperand::createReg(MO.getReg());
    return true;
  case MachineOperand::MO_Immediate:
    OutMO = MCOperand::createImm(MO.getImm());
    return true;
  case MachineOperand::MO_MachineBasicBlock:
    OutMO = MCOperand::createExpr(
        MCSymbolRefExpr::create(MO.getMBB()->getSymbol(), AP.OutContext));
    return true;
  case MachineOperand::MO_GlobalAddress:
  case MachineOperand::MO_ExternalSymbol:
    OutMO = GetSymbolRef(MO, GetSymbolFromOperand(MO, AP), AP);
    return true;
  case MachineOperand::MO_JumpTableIndex:
    OutMO = GetSymbolRef(MO, AP.GetJTISymbol(MO.getIndex()), AP);
    return true;
  case MachineOperand::MO_ConstantPoolIndex:
    OutMO = GetSymbolRef(MO, AP.GetCPISymbol(MO.getIndex()), AP);
    return true;
  case MachineOperand::MO_BlockAddress:
    OutMO =
        GetSymbolRef(MO, AP.GetBlockAddressSymbol(MO.getBlockAddress()), AP);
    return true;
  case MachineOperand::MO_MCSymbol:
    OutMO = GetSymbolRef(MO, MO.getMCSymbol(), AP);
    return true;
  case MachineOperand::MO_RegisterMask:
    return false;
  }
}
