//===-- RISCVInstrInfoXAMi.td - RISC-V 'AMi' instructions --*- tablegen -*-===//
//
// License:
//
//===----------------------------------------------------------------------===//
//
// This file describes the RISC-V mimicry instructions.
//
//===----------------------------------------------------------------------===//

def OPC_PLOAD     : RISCVOpcode<"PLOAD",     0b0000010>;
def OPC_OP_GIMM   : RISCVOpcode<"OP_GIMM",   0b0010000>;
def OPC_OP_MIMM   : RISCVOpcode<"OP_MIMM",   0b0010001>;
def OPC_OP_PIMM   : RISCVOpcode<"OP_PIMM",   0b0010010>;
def OPC_ABRANCH   : RISCVOpcode<"ABRANCH",   0b1100001>;
def OPC_AJALR     : RISCVOpcode<"AJALR",     0b1100101>;
def OPC_AJAL      : RISCVOpcode<"AJAL",      0b1101101>;
def OPC_MARK      : RISCVOpcode<"MARK",      0b0001011>;

//===----------------------------------------------------------------------===//
// Instruction Class Templates
//===----------------------------------------------------------------------===//

let hasSideEffects = 0, mayLoad = 0, mayStore = 0 in
class ABranchCC_rri<bits<3> funct3, string opcodestr>
    : RVInstB<funct3, OPC_ABRANCH, (outs),
              (ins GPR:$rs1, GPR:$rs2, simm13_lsb0:$imm12),
              opcodestr, "$rs1, $rs2, $imm12">,
      Sched<[WriteJmp, ReadJmp, ReadJmp]> {
  let isBranch = 1;
  let isTerminator = 1;
}

let hasSideEffects = 0, mayLoad = 1, mayStore = 0 in
class PLoad_ri<bits<3> funct3, string opcodestr>
    : RVInstI<funct3, OPC_PLOAD, (outs GPR:$rd), (ins GPR:$rs1, simm12:$imm12),
              opcodestr, "$rd, ${imm12}(${rs1})">;

let hasSideEffects = 0, mayLoad = 0, mayStore = 0 in
class MALU_ri<bits<3> funct3, string opcodestr>
    : RVInstI<funct3, OPC_OP_MIMM, (outs GPR:$rd), (ins GPR:$rs1, simm12:$imm12),
              opcodestr, "$rd, $rs1, $imm12">,
      Sched<[WriteIALU, ReadIALU]>;

let hasSideEffects = 0, mayLoad = 0, mayStore = 0 in
class GALU_ri<bits<3> funct3, string opcodestr>
    : RVInstI<funct3, OPC_OP_GIMM, (outs GPR:$rd), (ins GPR:$rs1, simm12:$imm12),
              opcodestr, "$rd, $rs1, $imm12">,
      Sched<[WriteIALU, ReadIALU]>;

let hasSideEffects = 0, mayLoad = 0, mayStore = 0 in
class PALU_ri<bits<3> funct3, string opcodestr>
    : RVInstI<funct3, OPC_OP_PIMM, (outs GPR:$rd), (ins GPR:$rs1, simm12:$imm12),
              opcodestr, "$rd, $rs1, $imm12">,
      Sched<[WriteIALU, ReadIALU]>;

//===----------------------------------------------------------------------===//
// Instructions
//===----------------------------------------------------------------------===//

let hasSideEffects = 0, mayLoad = 0, mayStore = 0 in {
let isCall = 1 in
def AJAL : RVInstJ<OPC_AJAL, (outs GPR:$rd), (ins simm21_lsb0_jal:$imm20),
                  "a.jal", "$rd, $imm20">, Sched<[WriteJal]>;

let isCall = 1 in
def AJALR : RVInstI<0b000, OPC_AJALR, (outs GPR:$rd),
                   (ins GPR:$rs1, simm12:$imm12),
                   "a.jalr", "$rd, ${imm12}(${rs1})">,
           Sched<[WriteJalr, ReadJalr]>;
}

def ABEQ : ABranchCC_rri<0b000, "a.beq">;
def ABNE : ABranchCC_rri<0b001, "a.bne">;

def PLW : PLoad_ri<0b010, "p.lw">, Sched<[WriteLDW, ReadMemBase]>;

def MADDI : MALU_ri<0b000, "m.addi">;
def GADDI : GALU_ri<0b000, "g.addi">;
def PADDI : PALU_ri<0b000, "p.addi">;

let hasSideEffects = 0, mayLoad = 0, mayStore = 0 in
def MARK : RVInstI<0b000, OPC_MARK, (outs),
                   (ins simm12:$imm12),
                   "mark", "${imm12}">, Sched<[]> {
  let rs1 = 0;
  let rd = 0;
}

//===----------------------------------------------------------------------===//
// Non-standard extensions
//===----------------------------------------------------------------------===//

def : InstAlias<"emm $offset", (ABEQ X0, X0, simm13_lsb0:$offset)>;
