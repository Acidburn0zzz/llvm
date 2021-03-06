//===- llvm/unittests/tools/llvm-cfi-verify/FileAnalysis.cpp --------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "../tools/llvm-cfi-verify/lib/FileAnalysis.h"
#include "../tools/llvm-cfi-verify/lib/GraphBuilder.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

#include "llvm/BinaryFormat/ELF.h"
#include "llvm/MC/MCAsmInfo.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCDisassembler/MCDisassembler.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCInstPrinter.h"
#include "llvm/MC/MCInstrAnalysis.h"
#include "llvm/MC/MCInstrDesc.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCObjectFileInfo.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/Object/Binary.h"
#include "llvm/Object/COFF.h"
#include "llvm/Object/ELFObjectFile.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/TargetRegistry.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdlib>

using Instr = ::llvm::cfi_verify::FileAnalysis::Instr;
using ::testing::Eq;
using ::testing::Field;

namespace llvm {
namespace cfi_verify {
namespace {
class ELFx86TestFileAnalysis : public FileAnalysis {
public:
  ELFx86TestFileAnalysis()
      : FileAnalysis(Triple("x86_64--"), SubtargetFeatures()) {}

  // Expose this method publicly for testing.
  void parseSectionContents(ArrayRef<uint8_t> SectionBytes,
                            uint64_t SectionAddress) {
    FileAnalysis::parseSectionContents(SectionBytes, SectionAddress);
  }

  Error initialiseDisassemblyMembers() {
    return FileAnalysis::initialiseDisassemblyMembers();
  }
};

class BasicFileAnalysisTest : public ::testing::Test {
protected:
  virtual void SetUp() {
    IgnoreDWARFFlag = true;
    SuccessfullyInitialised = true;
    if (auto Err = Analysis.initialiseDisassemblyMembers()) {
      handleAllErrors(std::move(Err), [&](const UnsupportedDisassembly &E) {
        SuccessfullyInitialised = false;
        outs()
            << "Note: CFIVerifyTests are disabled due to lack of x86 support "
               "on this build.\n";
      });
    }
  }

  bool SuccessfullyInitialised;
  ELFx86TestFileAnalysis Analysis;
};

TEST_F(BasicFileAnalysisTest, BasicDisassemblyTraversalTest) {
  if (!SuccessfullyInitialised)
    return;
  Analysis.parseSectionContents(
      {
          0x90,                   // 0: nop
          0xb0, 0x00,             // 1: mov $0x0, %al
          0x48, 0x89, 0xe5,       // 3: mov %rsp, %rbp
          0x48, 0x83, 0xec, 0x18, // 6: sub $0x18, %rsp
          0x48, 0xbe, 0xc4, 0x07, 0x40,
          0x00, 0x00, 0x00, 0x00, 0x00, // 10: movabs $0x4007c4, %rsi
          0x2f,                         // 20: (bad)
          0x41, 0x0e,                   // 21: rex.B (bad)
          0x62, 0x72, 0x65, 0x61, 0x6b, // 23: (bad) {%k1}
      },
      0xDEADBEEF);

  EXPECT_EQ(nullptr, Analysis.getInstruction(0x0));
  EXPECT_EQ(nullptr, Analysis.getInstruction(0x1000));

  // 0xDEADBEEF: nop
  const auto *InstrMeta = Analysis.getInstruction(0xDEADBEEF);
  EXPECT_NE(nullptr, InstrMeta);
  EXPECT_EQ(0xDEADBEEF, InstrMeta->VMAddress);
  EXPECT_EQ(1u, InstrMeta->InstructionSize);
  EXPECT_TRUE(InstrMeta->Valid);

  const auto *NextInstrMeta = Analysis.getNextInstructionSequential(*InstrMeta);
  EXPECT_EQ(nullptr, Analysis.getPrevInstructionSequential(*InstrMeta));
  const auto *PrevInstrMeta = InstrMeta;

  // 0xDEADBEEF + 1: mov $0x0, %al
  InstrMeta = Analysis.getInstruction(0xDEADBEEF + 1);
  EXPECT_NE(nullptr, InstrMeta);
  EXPECT_EQ(NextInstrMeta, InstrMeta);
  EXPECT_EQ(0xDEADBEEF + 1, InstrMeta->VMAddress);
  EXPECT_EQ(2u, InstrMeta->InstructionSize);
  EXPECT_TRUE(InstrMeta->Valid);

  NextInstrMeta = Analysis.getNextInstructionSequential(*InstrMeta);
  EXPECT_EQ(PrevInstrMeta, Analysis.getPrevInstructionSequential(*InstrMeta));
  PrevInstrMeta = InstrMeta;

  // 0xDEADBEEF + 3: mov %rsp, %rbp
  InstrMeta = Analysis.getInstruction(0xDEADBEEF + 3);
  EXPECT_NE(nullptr, InstrMeta);
  EXPECT_EQ(NextInstrMeta, InstrMeta);
  EXPECT_EQ(0xDEADBEEF + 3, InstrMeta->VMAddress);
  EXPECT_EQ(3u, InstrMeta->InstructionSize);
  EXPECT_TRUE(InstrMeta->Valid);

  NextInstrMeta = Analysis.getNextInstructionSequential(*InstrMeta);
  EXPECT_EQ(PrevInstrMeta, Analysis.getPrevInstructionSequential(*InstrMeta));
  PrevInstrMeta = InstrMeta;

  // 0xDEADBEEF + 6: sub $0x18, %rsp
  InstrMeta = Analysis.getInstruction(0xDEADBEEF + 6);
  EXPECT_NE(nullptr, InstrMeta);
  EXPECT_EQ(NextInstrMeta, InstrMeta);
  EXPECT_EQ(0xDEADBEEF + 6, InstrMeta->VMAddress);
  EXPECT_EQ(4u, InstrMeta->InstructionSize);
  EXPECT_TRUE(InstrMeta->Valid);

  NextInstrMeta = Analysis.getNextInstructionSequential(*InstrMeta);
  EXPECT_EQ(PrevInstrMeta, Analysis.getPrevInstructionSequential(*InstrMeta));
  PrevInstrMeta = InstrMeta;

  // 0xDEADBEEF + 10: movabs $0x4007c4, %rsi
  InstrMeta = Analysis.getInstruction(0xDEADBEEF + 10);
  EXPECT_NE(nullptr, InstrMeta);
  EXPECT_EQ(NextInstrMeta, InstrMeta);
  EXPECT_EQ(0xDEADBEEF + 10, InstrMeta->VMAddress);
  EXPECT_EQ(10u, InstrMeta->InstructionSize);
  EXPECT_TRUE(InstrMeta->Valid);

  EXPECT_EQ(nullptr, Analysis.getNextInstructionSequential(*InstrMeta));
  EXPECT_EQ(PrevInstrMeta, Analysis.getPrevInstructionSequential(*InstrMeta));
  PrevInstrMeta = InstrMeta;

  // 0xDEADBEEF + 20: (bad)
  InstrMeta = Analysis.getInstruction(0xDEADBEEF + 20);
  EXPECT_NE(nullptr, InstrMeta);
  EXPECT_EQ(0xDEADBEEF + 20, InstrMeta->VMAddress);
  EXPECT_EQ(1u, InstrMeta->InstructionSize);
  EXPECT_FALSE(InstrMeta->Valid);

  EXPECT_EQ(nullptr, Analysis.getNextInstructionSequential(*InstrMeta));
  EXPECT_EQ(PrevInstrMeta, Analysis.getPrevInstructionSequential(*InstrMeta));

  // 0xDEADBEEF + 21: rex.B (bad)
  InstrMeta = Analysis.getInstruction(0xDEADBEEF + 21);
  EXPECT_NE(nullptr, InstrMeta);
  EXPECT_EQ(0xDEADBEEF + 21, InstrMeta->VMAddress);
  EXPECT_EQ(2u, InstrMeta->InstructionSize);
  EXPECT_FALSE(InstrMeta->Valid);

  EXPECT_EQ(nullptr, Analysis.getNextInstructionSequential(*InstrMeta));
  EXPECT_EQ(nullptr, Analysis.getPrevInstructionSequential(*InstrMeta));

  // 0xDEADBEEF + 6: (bad) {%k1}
  InstrMeta = Analysis.getInstruction(0xDEADBEEF + 23);
  EXPECT_NE(nullptr, InstrMeta);
  EXPECT_EQ(0xDEADBEEF + 23, InstrMeta->VMAddress);
  EXPECT_EQ(5u, InstrMeta->InstructionSize);
  EXPECT_FALSE(InstrMeta->Valid);

  EXPECT_EQ(nullptr, Analysis.getNextInstructionSequential(*InstrMeta));
  EXPECT_EQ(nullptr, Analysis.getPrevInstructionSequential(*InstrMeta));
}

TEST_F(BasicFileAnalysisTest, PrevAndNextFromBadInst) {
  if (!SuccessfullyInitialised)
    return;
  Analysis.parseSectionContents(
      {
          0x90, // 0: nop
          0x2f, // 1: (bad)
          0x90  // 2: nop
      },
      0xDEADBEEF);
  const auto &BadInstrMeta = Analysis.getInstructionOrDie(0xDEADBEEF + 1);
  const auto *GoodInstrMeta =
      Analysis.getPrevInstructionSequential(BadInstrMeta);
  EXPECT_NE(nullptr, GoodInstrMeta);
  EXPECT_EQ(0xDEADBEEF, GoodInstrMeta->VMAddress);
  EXPECT_EQ(1u, GoodInstrMeta->InstructionSize);

  GoodInstrMeta = Analysis.getNextInstructionSequential(BadInstrMeta);
  EXPECT_NE(nullptr, GoodInstrMeta);
  EXPECT_EQ(0xDEADBEEF + 2, GoodInstrMeta->VMAddress);
  EXPECT_EQ(1u, GoodInstrMeta->InstructionSize);
}

TEST_F(BasicFileAnalysisTest, CFITrapTest) {
  if (!SuccessfullyInitialised)
    return;
  Analysis.parseSectionContents(
      {
          0x90,                   // 0: nop
          0xb0, 0x00,             // 1: mov $0x0, %al
          0x48, 0x89, 0xe5,       // 3: mov %rsp, %rbp
          0x48, 0x83, 0xec, 0x18, // 6: sub $0x18, %rsp
          0x48, 0xbe, 0xc4, 0x07, 0x40,
          0x00, 0x00, 0x00, 0x00, 0x00, // 10: movabs $0x4007c4, %rsi
          0x2f,                         // 20: (bad)
          0x41, 0x0e,                   // 21: rex.B (bad)
          0x62, 0x72, 0x65, 0x61, 0x6b, // 23: (bad) {%k1}
          0x0f, 0x0b                    // 28: ud2
      },
      0xDEADBEEF);

  EXPECT_FALSE(Analysis.isCFITrap(Analysis.getInstructionOrDie(0xDEADBEEF)));
  EXPECT_FALSE(
      Analysis.isCFITrap(Analysis.getInstructionOrDie(0xDEADBEEF + 3)));
  EXPECT_FALSE(
      Analysis.isCFITrap(Analysis.getInstructionOrDie(0xDEADBEEF + 6)));
  EXPECT_FALSE(
      Analysis.isCFITrap(Analysis.getInstructionOrDie(0xDEADBEEF + 10)));
  EXPECT_FALSE(
      Analysis.isCFITrap(Analysis.getInstructionOrDie(0xDEADBEEF + 20)));
  EXPECT_FALSE(
      Analysis.isCFITrap(Analysis.getInstructionOrDie(0xDEADBEEF + 21)));
  EXPECT_FALSE(
      Analysis.isCFITrap(Analysis.getInstructionOrDie(0xDEADBEEF + 23)));
  EXPECT_TRUE(
      Analysis.isCFITrap(Analysis.getInstructionOrDie(0xDEADBEEF + 28)));
}

TEST_F(BasicFileAnalysisTest, FallThroughTest) {
  if (!SuccessfullyInitialised)
    return;
  Analysis.parseSectionContents(
      {
          0x90,                         // 0: nop
          0xb0, 0x00,                   // 1: mov $0x0, %al
          0x2f,                         // 3: (bad)
          0x0f, 0x0b,                   // 4: ud2
          0xff, 0x20,                   // 6: jmpq *(%rax)
          0xeb, 0x00,                   // 8: jmp +0
          0xe8, 0x45, 0xfe, 0xff, 0xff, // 10: callq [some loc]
          0xff, 0x10,                   // 15: callq *(rax)
          0x75, 0x00,                   // 17: jne +0
          0xc3,                         // 19: retq
      },
      0xDEADBEEF);

  EXPECT_TRUE(
      Analysis.canFallThrough(Analysis.getInstructionOrDie(0xDEADBEEF)));
  EXPECT_TRUE(
      Analysis.canFallThrough(Analysis.getInstructionOrDie(0xDEADBEEF + 1)));
  EXPECT_FALSE(
      Analysis.canFallThrough(Analysis.getInstructionOrDie(0xDEADBEEF + 3)));
  EXPECT_FALSE(
      Analysis.canFallThrough(Analysis.getInstructionOrDie(0xDEADBEEF + 4)));
  EXPECT_FALSE(
      Analysis.canFallThrough(Analysis.getInstructionOrDie(0xDEADBEEF + 6)));
  EXPECT_FALSE(
      Analysis.canFallThrough(Analysis.getInstructionOrDie(0xDEADBEEF + 8)));
  EXPECT_FALSE(
      Analysis.canFallThrough(Analysis.getInstructionOrDie(0xDEADBEEF + 10)));
  EXPECT_FALSE(
      Analysis.canFallThrough(Analysis.getInstructionOrDie(0xDEADBEEF + 15)));
  EXPECT_TRUE(
      Analysis.canFallThrough(Analysis.getInstructionOrDie(0xDEADBEEF + 17)));
  EXPECT_FALSE(
      Analysis.canFallThrough(Analysis.getInstructionOrDie(0xDEADBEEF + 19)));
}

TEST_F(BasicFileAnalysisTest, DefiniteNextInstructionTest) {
  if (!SuccessfullyInitialised)
    return;
  Analysis.parseSectionContents(
      {
          0x90,                         // 0: nop
          0xb0, 0x00,                   // 1: mov $0x0, %al
          0x2f,                         // 3: (bad)
          0x0f, 0x0b,                   // 4: ud2
          0xff, 0x20,                   // 6: jmpq *(%rax)
          0xeb, 0x00,                   // 8: jmp 10 [+0]
          0xeb, 0x05,                   // 10: jmp 17 [+5]
          0xe8, 0x00, 0x00, 0x00, 0x00, // 12: callq 17 [+0]
          0xe8, 0x78, 0x56, 0x34, 0x12, // 17: callq 0x1234569f [+0x12345678]
          0xe8, 0x04, 0x00, 0x00, 0x00, // 22: callq 31 [+4]
          0xff, 0x10,                   // 27: callq *(rax)
          0x75, 0x00,                   // 29: jne 31 [+0]
          0x75, 0xe0,                   // 31: jne 1 [-32]
          0xc3,                         // 33: retq
          0xeb, 0xdd,                   // 34: jmp 1 [-35]
          0xeb, 0xdd,                   // 36: jmp 3 [-35]
          0xeb, 0xdc,                   // 38: jmp 4 [-36]
      },
      0xDEADBEEF);

  const auto *Current = Analysis.getInstruction(0xDEADBEEF);
  const auto *Next = Analysis.getDefiniteNextInstruction(*Current);
  EXPECT_NE(nullptr, Next);
  EXPECT_EQ(0xDEADBEEF + 1, Next->VMAddress);

  Current = Analysis.getInstruction(0xDEADBEEF + 1);
  EXPECT_EQ(nullptr, Analysis.getDefiniteNextInstruction(*Current));

  Current = Analysis.getInstruction(0xDEADBEEF + 3);
  EXPECT_EQ(nullptr, Analysis.getDefiniteNextInstruction(*Current));

  Current = Analysis.getInstruction(0xDEADBEEF + 4);
  EXPECT_EQ(nullptr, Analysis.getDefiniteNextInstruction(*Current));

  Current = Analysis.getInstruction(0xDEADBEEF + 6);
  EXPECT_EQ(nullptr, Analysis.getDefiniteNextInstruction(*Current));

  Current = Analysis.getInstruction(0xDEADBEEF + 8);
  Next = Analysis.getDefiniteNextInstruction(*Current);
  EXPECT_NE(nullptr, Next);
  EXPECT_EQ(0xDEADBEEF + 10, Next->VMAddress);

  Current = Analysis.getInstruction(0xDEADBEEF + 10);
  Next = Analysis.getDefiniteNextInstruction(*Current);
  EXPECT_NE(nullptr, Next);
  EXPECT_EQ(0xDEADBEEF + 17, Next->VMAddress);

  Current = Analysis.getInstruction(0xDEADBEEF + 12);
  Next = Analysis.getDefiniteNextInstruction(*Current);
  EXPECT_NE(nullptr, Next);
  EXPECT_EQ(0xDEADBEEF + 17, Next->VMAddress);

  Current = Analysis.getInstruction(0xDEADBEEF + 17);
  // Note, definite next instruction address is out of range and should fail.
  EXPECT_EQ(nullptr, Analysis.getDefiniteNextInstruction(*Current));
  Next = Analysis.getDefiniteNextInstruction(*Current);

  Current = Analysis.getInstruction(0xDEADBEEF + 22);
  Next = Analysis.getDefiniteNextInstruction(*Current);
  EXPECT_NE(nullptr, Next);
  EXPECT_EQ(0xDEADBEEF + 31, Next->VMAddress);

  Current = Analysis.getInstruction(0xDEADBEEF + 27);
  EXPECT_EQ(nullptr, Analysis.getDefiniteNextInstruction(*Current));
  Current = Analysis.getInstruction(0xDEADBEEF + 29);
  EXPECT_EQ(nullptr, Analysis.getDefiniteNextInstruction(*Current));
  Current = Analysis.getInstruction(0xDEADBEEF + 31);
  EXPECT_EQ(nullptr, Analysis.getDefiniteNextInstruction(*Current));
  Current = Analysis.getInstruction(0xDEADBEEF + 33);
  EXPECT_EQ(nullptr, Analysis.getDefiniteNextInstruction(*Current));

  Current = Analysis.getInstruction(0xDEADBEEF + 34);
  Next = Analysis.getDefiniteNextInstruction(*Current);
  EXPECT_NE(nullptr, Next);
  EXPECT_EQ(0xDEADBEEF + 1, Next->VMAddress);

  Current = Analysis.getInstruction(0xDEADBEEF + 36);
  EXPECT_EQ(nullptr, Analysis.getDefiniteNextInstruction(*Current));

  Current = Analysis.getInstruction(0xDEADBEEF + 38);
  Next = Analysis.getDefiniteNextInstruction(*Current);
  EXPECT_NE(nullptr, Next);
  EXPECT_EQ(0xDEADBEEF + 4, Next->VMAddress);
}

TEST_F(BasicFileAnalysisTest, ControlFlowXRefsTest) {
  if (!SuccessfullyInitialised)
    return;
  Analysis.parseSectionContents(
      {
          0x90,                         // 0: nop
          0xb0, 0x00,                   // 1: mov $0x0, %al
          0x2f,                         // 3: (bad)
          0x0f, 0x0b,                   // 4: ud2
          0xff, 0x20,                   // 6: jmpq *(%rax)
          0xeb, 0x00,                   // 8: jmp 10 [+0]
          0xeb, 0x05,                   // 10: jmp 17 [+5]
          0xe8, 0x00, 0x00, 0x00, 0x00, // 12: callq 17 [+0]
          0xe8, 0x78, 0x56, 0x34, 0x12, // 17: callq 0x1234569f [+0x12345678]
          0xe8, 0x04, 0x00, 0x00, 0x00, // 22: callq 31 [+4]
          0xff, 0x10,                   // 27: callq *(rax)
          0x75, 0x00,                   // 29: jne 31 [+0]
          0x75, 0xe0,                   // 31: jne 1 [-32]
          0xc3,                         // 33: retq
          0xeb, 0xdd,                   // 34: jmp 1 [-35]
          0xeb, 0xdd,                   // 36: jmp 3 [-35]
          0xeb, 0xdc,                   // 38: jmp 4 [-36]
      },
      0xDEADBEEF);
  const auto *InstrMetaPtr = &Analysis.getInstructionOrDie(0xDEADBEEF);
  std::set<const Instr *> XRefs =
      Analysis.getDirectControlFlowXRefs(*InstrMetaPtr);
  EXPECT_TRUE(XRefs.empty());

  InstrMetaPtr = &Analysis.getInstructionOrDie(0xDEADBEEF + 1);
  XRefs = Analysis.getDirectControlFlowXRefs(*InstrMetaPtr);
  EXPECT_THAT(XRefs, UnorderedElementsAre(
                         Field(&Instr::VMAddress, Eq(0xDEADBEEF)),
                         Field(&Instr::VMAddress, Eq(0xDEADBEEF + 31)),
                         Field(&Instr::VMAddress, Eq(0xDEADBEEF + 34))));

  InstrMetaPtr = &Analysis.getInstructionOrDie(0xDEADBEEF + 3);
  XRefs = Analysis.getDirectControlFlowXRefs(*InstrMetaPtr);
  EXPECT_THAT(XRefs, UnorderedElementsAre(
                         Field(&Instr::VMAddress, Eq(0xDEADBEEF + 1)),
                         Field(&Instr::VMAddress, Eq(0xDEADBEEF + 36))));

  InstrMetaPtr = &Analysis.getInstructionOrDie(0xDEADBEEF + 4);
  XRefs = Analysis.getDirectControlFlowXRefs(*InstrMetaPtr);
  EXPECT_THAT(XRefs, UnorderedElementsAre(
                         Field(&Instr::VMAddress, Eq(0xDEADBEEF + 38))));

  InstrMetaPtr = &Analysis.getInstructionOrDie(0xDEADBEEF + 6);
  EXPECT_TRUE(Analysis.getDirectControlFlowXRefs(*InstrMetaPtr).empty());

  InstrMetaPtr = &Analysis.getInstructionOrDie(0xDEADBEEF + 8);
  XRefs = Analysis.getDirectControlFlowXRefs(*InstrMetaPtr);
  EXPECT_TRUE(Analysis.getDirectControlFlowXRefs(*InstrMetaPtr).empty());

  InstrMetaPtr = &Analysis.getInstructionOrDie(0xDEADBEEF + 10);
  XRefs = Analysis.getDirectControlFlowXRefs(*InstrMetaPtr);
  EXPECT_THAT(XRefs, UnorderedElementsAre(
                         Field(&Instr::VMAddress, Eq(0xDEADBEEF + 8))));

  InstrMetaPtr = &Analysis.getInstructionOrDie(0xDEADBEEF + 12);
  XRefs = Analysis.getDirectControlFlowXRefs(*InstrMetaPtr);
  EXPECT_TRUE(Analysis.getDirectControlFlowXRefs(*InstrMetaPtr).empty());

  InstrMetaPtr = &Analysis.getInstructionOrDie(0xDEADBEEF + 17);
  XRefs = Analysis.getDirectControlFlowXRefs(*InstrMetaPtr);
  EXPECT_THAT(XRefs, UnorderedElementsAre(
                         Field(&Instr::VMAddress, Eq(0xDEADBEEF + 10)),
                         Field(&Instr::VMAddress, Eq(0xDEADBEEF + 12))));

  InstrMetaPtr = &Analysis.getInstructionOrDie(0xDEADBEEF + 22);
  XRefs = Analysis.getDirectControlFlowXRefs(*InstrMetaPtr);
  EXPECT_TRUE(Analysis.getDirectControlFlowXRefs(*InstrMetaPtr).empty());

  InstrMetaPtr = &Analysis.getInstructionOrDie(0xDEADBEEF + 27);
  XRefs = Analysis.getDirectControlFlowXRefs(*InstrMetaPtr);
  EXPECT_TRUE(Analysis.getDirectControlFlowXRefs(*InstrMetaPtr).empty());

  InstrMetaPtr = &Analysis.getInstructionOrDie(0xDEADBEEF + 29);
  XRefs = Analysis.getDirectControlFlowXRefs(*InstrMetaPtr);
  EXPECT_TRUE(Analysis.getDirectControlFlowXRefs(*InstrMetaPtr).empty());

  InstrMetaPtr = &Analysis.getInstructionOrDie(0xDEADBEEF + 31);
  XRefs = Analysis.getDirectControlFlowXRefs(*InstrMetaPtr);
  EXPECT_THAT(XRefs, UnorderedElementsAre(
                         Field(&Instr::VMAddress, Eq(0xDEADBEEF + 22)),
                         Field(&Instr::VMAddress, Eq(0xDEADBEEF + 29))));

  InstrMetaPtr = &Analysis.getInstructionOrDie(0xDEADBEEF + 33);
  XRefs = Analysis.getDirectControlFlowXRefs(*InstrMetaPtr);
  EXPECT_THAT(XRefs, UnorderedElementsAre(
                         Field(&Instr::VMAddress, Eq(0xDEADBEEF + 31))));

  InstrMetaPtr = &Analysis.getInstructionOrDie(0xDEADBEEF + 34);
  XRefs = Analysis.getDirectControlFlowXRefs(*InstrMetaPtr);
  EXPECT_TRUE(Analysis.getDirectControlFlowXRefs(*InstrMetaPtr).empty());

  InstrMetaPtr = &Analysis.getInstructionOrDie(0xDEADBEEF + 36);
  XRefs = Analysis.getDirectControlFlowXRefs(*InstrMetaPtr);
  EXPECT_TRUE(Analysis.getDirectControlFlowXRefs(*InstrMetaPtr).empty());

  InstrMetaPtr = &Analysis.getInstructionOrDie(0xDEADBEEF + 38);
  XRefs = Analysis.getDirectControlFlowXRefs(*InstrMetaPtr);
  EXPECT_TRUE(Analysis.getDirectControlFlowXRefs(*InstrMetaPtr).empty());
}

TEST_F(BasicFileAnalysisTest, CFIProtectionInvalidTargets) {
  if (!SuccessfullyInitialised)
    return;
  Analysis.parseSectionContents(
      {
          0x90,       // 0: nop
          0x0f, 0x0b, // 1: ud2
          0x75, 0x00, // 3: jne 5 [+0]
      },
      0xDEADBEEF);
  EXPECT_FALSE(Analysis.isIndirectInstructionCFIProtected(0xDEADBEEF));
  EXPECT_FALSE(Analysis.isIndirectInstructionCFIProtected(0xDEADBEEF + 1));
  EXPECT_FALSE(Analysis.isIndirectInstructionCFIProtected(0xDEADBEEF + 3));
  EXPECT_FALSE(Analysis.isIndirectInstructionCFIProtected(0xDEADC0DE));
}

TEST_F(BasicFileAnalysisTest, CFIProtectionBasicFallthroughToUd2) {
  if (!SuccessfullyInitialised)
    return;
  Analysis.parseSectionContents(
      {
          0x75, 0x02, // 0: jne 4 [+2]
          0x0f, 0x0b, // 2: ud2
          0xff, 0x10, // 4: callq *(%rax)
      },
      0xDEADBEEF);
  EXPECT_TRUE(Analysis.isIndirectInstructionCFIProtected(0xDEADBEEF + 4));
}

TEST_F(BasicFileAnalysisTest, CFIProtectionBasicJumpToUd2) {
  if (!SuccessfullyInitialised)
    return;
  Analysis.parseSectionContents(
      {
          0x75, 0x02, // 0: jne 4 [+2]
          0xff, 0x10, // 2: callq *(%rax)
          0x0f, 0x0b, // 4: ud2
      },
      0xDEADBEEF);
  EXPECT_TRUE(Analysis.isIndirectInstructionCFIProtected(0xDEADBEEF + 2));
}

TEST_F(BasicFileAnalysisTest, CFIProtectionDualPathUd2) {
  if (!SuccessfullyInitialised)
    return;
  Analysis.parseSectionContents(
      {
          0x75, 0x03, // 0: jne 5 [+3]
          0x90,       // 2: nop
          0xff, 0x10, // 3: callq *(%rax)
          0x0f, 0x0b, // 5: ud2
          0x75, 0xf9, // 7: jne 2 [-7]
          0x0f, 0x0b, // 9: ud2
      },
      0xDEADBEEF);
  EXPECT_TRUE(Analysis.isIndirectInstructionCFIProtected(0xDEADBEEF + 3));
}

TEST_F(BasicFileAnalysisTest, CFIProtectionDualPathSingleUd2) {
  if (!SuccessfullyInitialised)
    return;
  Analysis.parseSectionContents(
      {
          0x75, 0x05, // 0: jne 7 [+5]
          0x90,       // 2: nop
          0xff, 0x10, // 3: callq *(%rax)
          0x75, 0xfb, // 5: jne 2 [-5]
          0x0f, 0x0b, // 7: ud2
      },
      0xDEADBEEF);
  EXPECT_TRUE(Analysis.isIndirectInstructionCFIProtected(0xDEADBEEF + 3));
}

TEST_F(BasicFileAnalysisTest, CFIProtectionDualFailLimitUpwards) {
  if (!SuccessfullyInitialised)
    return;
  Analysis.parseSectionContents(
      {
          0x75, 0x06, // 0: jne 8 [+6]
          0x90,       // 2: nop
          0x90,       // 3: nop
          0x90,       // 4: nop
          0x90,       // 5: nop
          0xff, 0x10, // 6: callq *(%rax)
          0x0f, 0x0b, // 8: ud2
      },
      0xDEADBEEF);
  uint64_t PrevSearchLengthForConditionalBranch =
      SearchLengthForConditionalBranch;
  SearchLengthForConditionalBranch = 2;

  EXPECT_FALSE(Analysis.isIndirectInstructionCFIProtected(0xDEADBEEF + 6));

  SearchLengthForConditionalBranch = PrevSearchLengthForConditionalBranch;
}

TEST_F(BasicFileAnalysisTest, CFIProtectionDualFailLimitDownwards) {
  if (!SuccessfullyInitialised)
    return;
  Analysis.parseSectionContents(
      {
          0x75, 0x02, // 0: jne 4 [+2]
          0xff, 0x10, // 2: callq *(%rax)
          0x90,       // 4: nop
          0x90,       // 5: nop
          0x90,       // 6: nop
          0x90,       // 7: nop
          0x0f, 0x0b, // 8: ud2
      },
      0xDEADBEEF);
  uint64_t PrevSearchLengthForUndef = SearchLengthForUndef;
  SearchLengthForUndef = 2;

  EXPECT_FALSE(Analysis.isIndirectInstructionCFIProtected(0xDEADBEEF + 2));

  SearchLengthForUndef = PrevSearchLengthForUndef;
}

TEST_F(BasicFileAnalysisTest, CFIProtectionGoodAndBadPaths) {
  if (!SuccessfullyInitialised)
    return;
  Analysis.parseSectionContents(
      {
          0xeb, 0x02, // 0: jmp 4 [+2]
          0x75, 0x02, // 2: jne 6 [+2]
          0xff, 0x10, // 4: callq *(%rax)
          0x0f, 0x0b, // 6: ud2
      },
      0xDEADBEEF);
  EXPECT_FALSE(Analysis.isIndirectInstructionCFIProtected(0xDEADBEEF + 4));
}

TEST_F(BasicFileAnalysisTest, CFIProtectionWithUnconditionalJumpInFallthrough) {
  if (!SuccessfullyInitialised)
    return;
  Analysis.parseSectionContents(
      {
          0x75, 0x04, // 0: jne 6 [+4]
          0xeb, 0x00, // 2: jmp 4 [+0]
          0xff, 0x10, // 4: callq *(%rax)
          0x0f, 0x0b, // 6: ud2
      },
      0xDEADBEEF);
  EXPECT_TRUE(Analysis.isIndirectInstructionCFIProtected(0xDEADBEEF + 4));
}

TEST_F(BasicFileAnalysisTest, CFIProtectionComplexExample) {
  if (!SuccessfullyInitialised)
    return;
  // See unittests/GraphBuilder.cpp::BuildFlowGraphComplexExample for this
  // graph.
  Analysis.parseSectionContents(
      {
          0x75, 0x12,                   // 0: jne 20 [+18]
          0xeb, 0x03,                   // 2: jmp 7 [+3]
          0x75, 0x10,                   // 4: jne 22 [+16]
          0x90,                         // 6: nop
          0x90,                         // 7: nop
          0x90,                         // 8: nop
          0xff, 0x10,                   // 9: callq *(%rax)
          0xeb, 0xfc,                   // 11: jmp 9 [-4]
          0x75, 0xfa,                   // 13: jne 9 [-6]
          0xe8, 0x78, 0x56, 0x34, 0x12, // 15: callq OUTOFBOUNDS [+0x12345678]
          0x90,                         // 20: nop
          0x90,                         // 21: nop
          0x0f, 0x0b,                   // 22: ud2
      },
      0xDEADBEEF);
  uint64_t PrevSearchLengthForUndef = SearchLengthForUndef;
  SearchLengthForUndef = 5;
  EXPECT_FALSE(Analysis.isIndirectInstructionCFIProtected(0xDEADBEEF + 9));
  SearchLengthForUndef = PrevSearchLengthForUndef;
}

TEST_F(BasicFileAnalysisTest, UndefSearchLengthOneTest) {
  Analysis.parseSectionContents(
      {
          0x77, 0x0d,                   // 0x688118: ja 0x688127 [+12]
          0x48, 0x89, 0xdf,             // 0x68811a: mov %rbx, %rdi
          0xff, 0xd0,                   // 0x68811d: callq *%rax
          0x48, 0x89, 0xdf,             // 0x68811f: mov %rbx, %rdi
          0xe8, 0x09, 0x00, 0x00, 0x00, // 0x688122: callq 0x688130
          0x0f, 0x0b,                   // 0x688127: ud2
      },
      0x688118);
  uint64_t PrevSearchLengthForUndef = SearchLengthForUndef;
  SearchLengthForUndef = 1;
  EXPECT_TRUE(Analysis.isIndirectInstructionCFIProtected(0x68811d));
  SearchLengthForUndef = PrevSearchLengthForUndef;
}

TEST_F(BasicFileAnalysisTest, UndefSearchLengthOneTestFarAway) {
  Analysis.parseSectionContents(
      {
          0x74, 0x73,                         // 0x7759eb: je 0x775a60
          0xe9, 0x1c, 0x04, 0x00, 0x00, 0x00, // 0x7759ed: jmpq 0x775e0e
      },
      0x7759eb);

  Analysis.parseSectionContents(
      {
          0x0f, 0x85, 0xb2, 0x03, 0x00, 0x00, // 0x775a56: jne    0x775e0e
          0x48, 0x83, 0xc3, 0xf4, // 0x775a5c: add    $0xfffffffffffffff4,%rbx
          0x48, 0x8b, 0x7c, 0x24, 0x10, // 0x775a60: mov    0x10(%rsp),%rdi
          0x48, 0x89, 0xde,             // 0x775a65: mov    %rbx,%rsi
          0xff, 0xd1,                   // 0x775a68: callq  *%rcx
      },
      0x775a56);

  Analysis.parseSectionContents(
      {
          0x0f, 0x0b, // 0x775e0e: ud2
      },
      0x775e0e);
  uint64_t PrevSearchLengthForUndef = SearchLengthForUndef;
  SearchLengthForUndef = 1;
  EXPECT_FALSE(Analysis.isIndirectInstructionCFIProtected(0x775a68));
  SearchLengthForUndef = 2;
  EXPECT_TRUE(Analysis.isIndirectInstructionCFIProtected(0x775a68));
  SearchLengthForUndef = 3;
  EXPECT_TRUE(Analysis.isIndirectInstructionCFIProtected(0x775a68));
  SearchLengthForUndef = PrevSearchLengthForUndef;
}

} // anonymous namespace
} // end namespace cfi_verify
} // end namespace llvm

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  llvm::cl::ParseCommandLineOptions(argc, argv);

  llvm::InitializeAllTargetInfos();
  llvm::InitializeAllTargetMCs();
  llvm::InitializeAllAsmParsers();
  llvm::InitializeAllDisassemblers();

  return RUN_ALL_TESTS();
}
