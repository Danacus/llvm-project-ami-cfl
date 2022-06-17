//===-- RISCVInstrInfoXAMi.td - RISC-V 'AMi' instructions --*- tablegen -*-===//
//
// License:
//
//===----------------------------------------------------------------------===//
//
// This file describes the RISC-V mimicry instructions.
//
//===----------------------------------------------------------------------===//

def OPC_MLOAD     : RISCVOpcode<"MLOAD",     0b0000001>;
def OPC_PLOAD     : RISCVOpcode<"PLOAD",     0b0000010>;
def OPC_GLOAD     : RISCVOpcode<"GLOAD",     0b0000000>;
def OPC_OP_GIMM   : RISCVOpcode<"OP_GIMM",   0b0010000>;
def OPC_OP_MIMM   : RISCVOpcode<"OP_MIMM",   0b0010001>;
def OPC_OP_PIMM   : RISCVOpcode<"OP_PIMM",   0b0010010>;
def OPC_OPM       : RISCVOpcode<"OPM",       0b0110001>;
def OPC_OPP       : RISCVOpcode<"OPM",       0b0110010>;
def OPC_ABRANCH   : RISCVOpcode<"ABRANCH",   0b1100001>;
def OPC_CTBRANCH  : RISCVOpcode<"CTBRANCH",  0b1100000>;
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

let hasSideEffects = 0, mayLoad = 0, mayStore = 0 in
class CTBranchCC_rri<bits<3> funct3, string opcodestr>
    : RVInstB<funct3, OPC_CTBRANCH, (outs),
              (ins GPR:$rs1, GPR:$rs2, simm13_lsb0:$imm12),
              opcodestr, "$rs1, $rs2, $imm12">,
      Sched<[WriteJmp, ReadJmp, ReadJmp]> {
  let isBranch = 1;
  let isTerminator = 1;
}

let hasSideEffects = 0, mayLoad = 1, mayStore = 0 in {
class MLoad_ri<bits<3> funct3, string opcodestr>
    : RVInstI<funct3, OPC_MLOAD, (outs GPR:$rd), (ins GPR:$rs1, simm12:$imm12),
              opcodestr, "$rd, ${imm12}(${rs1})">;

class PLoad_ri<bits<3> funct3, string opcodestr>
    : RVInstI<funct3, OPC_PLOAD, (outs GPR:$rd), (ins GPR:$rs1, simm12:$imm12),
              opcodestr, "$rd, ${imm12}(${rs1})">;
class GLoad_ri<bits<3> funct3, string opcodestr>
    : RVInstI<funct3, OPC_GLOAD, (outs GPR:$rd), (ins GPR:$rs1, simm12:$imm12),
              opcodestr, "$rd, ${imm12}(${rs1})">;
}

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

let hasSideEffects = 0, mayLoad = 0, mayStore = 0 in
class MALU_rr<bits<7> funct7, bits<3> funct3, string opcodestr,
             bit Commutable = 0>
    : RVInstR<funct7, funct3, OPC_OPM, (outs GPR:$rd), (ins GPR:$rs1, GPR:$rs2),
              opcodestr, "$rd, $rs1, $rs2"> {
  let isCommutable = Commutable;
}

let hasSideEffects = 0, mayLoad = 0, mayStore = 0 in
class PALU_rr<bits<7> funct7, bits<3> funct3, string opcodestr,
             bit Commutable = 0>
    : RVInstR<funct7, funct3, OPC_OPP, (outs GPR:$rd), (ins GPR:$rs1, GPR:$rs2),
              opcodestr, "$rd, $rs1, $rs2"> {
  let isCommutable = Commutable;
}


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

def ABEQ  : ABranchCC_rri<0b000, "a.beq">;
def ABNE  : ABranchCC_rri<0b001, "a.bne">;
def ABLT  : ABranchCC_rri<0b100, "a.blt">;
def ABGE  : ABranchCC_rri<0b101, "a.bge">;
def ABLTU : ABranchCC_rri<0b110, "a.bltu">;
def ABGEU : ABranchCC_rri<0b111, "a.bgeu">;

def CTBEQ  : CTBranchCC_rri<0b000, "ct.beq">;
def CTBNE  : CTBranchCC_rri<0b001, "ct.bne">;
def CTBLT  : CTBranchCC_rri<0b100, "ct.blt">;
def CTBGE  : CTBranchCC_rri<0b101, "ct.bge">;
def CTBLTU : CTBranchCC_rri<0b110, "ct.bltu">;
def CTBGEU : CTBranchCC_rri<0b111, "ct.bgeu">;

def GLB : GLoad_ri<0b000, "g.lb">, Sched<[WriteLDB, ReadMemBase]>;
def MLW : MLoad_ri<0b010, "m.lw">, Sched<[WriteLDW, ReadMemBase]>;
def PLW : PLoad_ri<0b010, "p.lw">, Sched<[WriteLDW, ReadMemBase]>;
def GLW : GLoad_ri<0b010, "g.lw">, Sched<[WriteLDW, ReadMemBase]>;
def MLBU: MLoad_ri<0b100, "m.lbu">, Sched<[WriteLDB, ReadMemBase]>;

let isReMaterializable = 1, isAsCheapAsAMove = 1 in {
def MADDI : MALU_ri<0b000, "m.addi">;
def GADDI : GALU_ri<0b000, "g.addi">;
def PADDI : PALU_ri<0b000, "p.addi">;
}

let isReMaterializable = 1, isAsCheapAsAMove = 1 in {
def MORI  : MALU_ri<0b110, "m.ori">;
}


def MADD  : MALU_rr<0b0000000, 0b000, "m.add", /*Commutable*/1>,
            Sched<[WriteIALU, ReadIALU, ReadIALU]>;
def PADD  : PALU_rr<0b0000000, 0b000, "p.add", /*Commutable*/1>,
            Sched<[WriteIALU, ReadIALU, ReadIALU]>;

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
