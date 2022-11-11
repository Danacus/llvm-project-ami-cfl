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
def OPC_PLUI      : RISCVOpcode<"PLUI",      0b0110110>;
def OPC_ABRANCH   : RISCVOpcode<"ABRANCH",   0b1100001>;
def OPC_CTBRANCH  : RISCVOpcode<"CTBRANCH",  0b1100000>;
def OPC_AJALR     : RISCVOpcode<"AJALR",     0b1100101>;
def OPC_AJAL      : RISCVOpcode<"AJAL",      0b1101101>;
def OPC_MARK      : RISCVOpcode<"MARK",      0b0001011>;




//===----------------------------------------------------------------------===//
// Instruction Class Templates
//===----------------------------------------------------------------------===//

class AMiClass;

def Mimicable: AMiClass;
def Activatable: AMiClass;
def AlwaysPersistent: AMiClass;

class AMiInstQualified<string I, string QI, string Q> {
  string Inst = I;
  string QualInst = QI;
  string Qualifier = Q;
  AMiClass Class = Mimicable;
}

class AMiQualified<string I, string Q> : AMiInstQualified<!substr(I, !size(Q)), I, Q>;
class AMiQualifiedAuto<string I, int quallen = 1> : AMiInstQualified<!substr(I, quallen), I, !substr(I, 0, quallen)>;
class AMiAlwaysPersistent<string I> : AMiInstQualified<I, I, "p"> {
  let Class = AlwaysPersistent;
}

multiclass QualifiedMulti<string qualifier> {
  def _m : AMiQualified<NAME, qualifier>;
}

multiclass ABranchCC_rri<bits<3> funct3, string opcodestr> {
  let hasSideEffects = 0, mayLoad = 0, mayStore = 0 in
  def NAME : RVInstB<funct3, OPC_ABRANCH, (outs),
            (ins GPR:$rs1, GPR:$rs2, simm13_lsb0:$imm12),
            opcodestr, "$rs1, $rs2, $imm12">,
      Sched<[WriteJmp, ReadJmp, ReadJmp]> {
    let isBranch = 1;
    let isTerminator = 1;
  }
  def _m : AMiQualified<NAME, "a"> {
    let Class = Activatable; 
  }
}

multiclass CTBranchCC_rri<bits<3> funct3, string opcodestr> {
  let hasSideEffects = 0, mayLoad = 0, mayStore = 0 in
  def NAME : RVInstB<funct3, OPC_CTBRANCH, (outs),
              (ins GPR:$rs1, GPR:$rs2, simm13_lsb0:$imm12),
              opcodestr, "$rs1, $rs2, $imm12">,
      Sched<[WriteJmp, ReadJmp, ReadJmp]> {
    let isBranch = 1;
    let isTerminator = 1;
  }
  def _m : AMiQualified<NAME, "ct"> {
    let Class = Activatable; 
  }
}

let hasSideEffects = 0, mayLoad = 1, mayStore = 0 in {
multiclass MLoad_ri<bits<3> funct3, string opcodestr> : QualifiedMulti<"m"> {
  def NAME : RVInstI<funct3, OPC_MLOAD, (outs GPR:$rd), (ins GPR:$rs1, simm12:$imm12),
              opcodestr, "$rd, ${imm12}(${rs1})">;
}
multiclass PLoad_ri<bits<3> funct3, string opcodestr> : QualifiedMulti<"p"> {
  def NAME : RVInstI<funct3, OPC_PLOAD, (outs GPR:$rd), (ins GPR:$rs1, simm12:$imm12),
              opcodestr, "$rd, ${imm12}(${rs1})">;
}
multiclass GLoad_ri<bits<3> funct3, string opcodestr> : QualifiedMulti<"g"> {
  def NAME : RVInstI<funct3, OPC_GLOAD, (outs GPR:$rd), (ins GPR:$rs1, simm12:$imm12),              
        opcodestr, "$rd, ${imm12}(${rs1})">;
}
}

let hasSideEffects = 0, mayLoad = 0, mayStore = 0 in {
class MALU_ri<bits<3> funct3, string opcodestr>
    : RVInstI<funct3, OPC_OP_MIMM, (outs GPR:$rd), (ins GPR:$rs1, simm12:$imm12),
              opcodestr, "$rd, $rs1, $imm12">,
      Sched<[WriteIALU, ReadIALU]>;

class GALU_ri<bits<3> funct3, string opcodestr>
    : RVInstI<funct3, OPC_OP_GIMM, (outs GPR:$rd), (ins GPR:$rs1, simm12:$imm12),
              opcodestr, "$rd, $rs1, $imm12">,
      Sched<[WriteIALU, ReadIALU]>;

class PALU_ri<bits<3> funct3, string opcodestr>
    : RVInstI<funct3, OPC_OP_PIMM, (outs GPR:$rd), (ins GPR:$rs1, simm12:$imm12),
              opcodestr, "$rd, $rs1, $imm12">,
      Sched<[WriteIALU, ReadIALU]>;

class MALU_rr<bits<7> funct7, bits<3> funct3, string opcodestr,
             bit Commutable = 0>
    : RVInstR<funct7, funct3, OPC_OPM, (outs GPR:$rd), (ins GPR:$rs1, GPR:$rs2),
              opcodestr, "$rd, $rs1, $rs2"> {
  let isCommutable = Commutable;
}
}

let hasSideEffects = 0, mayLoad = 0, mayStore = 0 in
class PALU_rr<bits<7> funct7, bits<3> funct3, string opcodestr,
             bit Commutable = 0>
    : RVInstR<funct7, funct3, OPC_OPP, (outs GPR:$rd), (ins GPR:$rs1, GPR:$rs2),
              opcodestr, "$rd, $rs1, $rs2"> {
  let isCommutable = Commutable;
}

let hasSideEffects = 0, mayLoad = 0, mayStore = 0 in {
multiclass MShift_ri<bits<5> imm11_7, bits<3> funct3, string opcodestr> : QualifiedMulti<"m"> {
  def NAME : RVInstIShift<imm11_7, funct3, OPC_OP_MIMM, (outs GPR:$rd),
                   (ins GPR:$rs1, uimmlog2xlen:$shamt), opcodestr,
                   "$rd, $rs1, $shamt">,
      Sched<[WriteShiftImm, ReadShiftImm]>;
}

multiclass PShift_ri<bits<5> imm11_7, bits<3> funct3, string opcodestr> : QualifiedMulti<"p"> {
  def NAME : RVInstIShift<imm11_7, funct3, OPC_OP_PIMM, (outs GPR:$rd),
                   (ins GPR:$rs1, uimmlog2xlen:$shamt), opcodestr,
                   "$rd, $rs1, $shamt">,
      Sched<[WriteShiftImm, ReadShiftImm]>;
}

multiclass GShift_ri<bits<5> imm11_7, bits<3> funct3, string opcodestr> : QualifiedMulti<"g"> {
  def NAME : RVInstIShift<imm11_7, funct3, OPC_OP_GIMM, (outs GPR:$rd),
                   (ins GPR:$rs1, uimmlog2xlen:$shamt), opcodestr,
                   "$rd, $rs1, $shamt">,
      Sched<[WriteShiftImm, ReadShiftImm]>;
}
}

//===----------------------------------------------------------------------===//
// Instructions
//===----------------------------------------------------------------------===//

let hasSideEffects = 0, mayLoad = 0, mayStore = 0 in {
let isReMaterializable = 1, isAsCheapAsAMove = 1 in
def PLUI : RVInstU<OPC_PLUI, (outs GPR:$rd), (ins uimm20_lui:$imm20),
                  "p.lui", "$rd, $imm20">, Sched<[WriteIALU]>;

let isCall = 1 in
def AJAL : RVInstJ<OPC_AJAL, (outs GPR:$rd), (ins simm21_lsb0_jal:$imm20),
                  "a.jal", "$rd, $imm20">, Sched<[WriteJal]>;

let isCall = 1 in
def AJALR : RVInstI<0b000, OPC_AJALR, (outs GPR:$rd),
                   (ins GPR:$rs1, simm12:$imm12),
                   "a.jalr", "$rd, ${imm12}(${rs1})">,
           Sched<[WriteJalr, ReadJalr]>;
}

def : AMiQualifiedAuto<"PLUI">;
def : AMiQualifiedAuto<"AJAL"> {
  let Class = Activatable;
}
def : AMiQualifiedAuto<"AJALR"> {
  let Class = Activatable;
}

defm ABEQ  : ABranchCC_rri<0b000, "a.beq">;
defm ABNE  : ABranchCC_rri<0b001, "a.bne">;
defm ABLT  : ABranchCC_rri<0b100, "a.blt">;
defm ABGE  : ABranchCC_rri<0b101, "a.bge">;
defm ABLTU : ABranchCC_rri<0b110, "a.bltu">;
defm ABGEU : ABranchCC_rri<0b111, "a.bgeu">;

defm CTBEQ  : CTBranchCC_rri<0b000, "ct.beq">;
defm CTBNE  : CTBranchCC_rri<0b001, "ct.bne">;
defm CTBLT  : CTBranchCC_rri<0b100, "ct.blt">;
defm CTBGE  : CTBranchCC_rri<0b101, "ct.bge">;
defm CTBLTU : CTBranchCC_rri<0b110, "ct.bltu">;
defm CTBGEU : CTBranchCC_rri<0b111, "ct.bgeu">;

defm GLB : GLoad_ri<0b000, "g.lb">, Sched<[WriteLDB, ReadMemBase]>;
defm MLW : MLoad_ri<0b010, "m.lw">, Sched<[WriteLDW, ReadMemBase]>;
defm PLW : PLoad_ri<0b010, "p.lw">, Sched<[WriteLDW, ReadMemBase]>;
defm GLW : GLoad_ri<0b010, "g.lw">, Sched<[WriteLDW, ReadMemBase]>;
defm MLBU: MLoad_ri<0b100, "m.lbu">, Sched<[WriteLDB, ReadMemBase]>;

let isReMaterializable = 1, isAsCheapAsAMove = 1 in {
def MADDI : MALU_ri<0b000, "m.addi">;
def GADDI : GALU_ri<0b000, "g.addi">;
def PADDI : PALU_ri<0b000, "p.addi">;
}

def : AMiQualifiedAuto<"MADDI">;
def : AMiQualifiedAuto<"GADDI">;
def : AMiQualifiedAuto<"PADDI">;

let isReMaterializable = 1, isAsCheapAsAMove = 1 in {
def MORI  : MALU_ri<0b110, "m.ori">;
}

def : AMiQualifiedAuto<"MORI">;

def MANDI  : MALU_ri<0b111, "m.andi">;
def : AMiQualifiedAuto<"MANDI">;
def PANDI  : PALU_ri<0b111, "p.andi">;
def : AMiQualifiedAuto<"PANDI">;
def GANDI  : GALU_ri<0b111, "g.andi">;
def : AMiQualifiedAuto<"GANDI">;

defm GSLLI : GShift_ri<0b00000, 0b001, "g.slli">;
defm MSLLI : MShift_ri<0b00000, 0b001, "m.slli">;
defm PSLLI : PShift_ri<0b00000, 0b001, "p.slli">;

defm GSRLI : GShift_ri<0b00000, 0b101, "g.srli">;
defm MSRLI : MShift_ri<0b00000, 0b101, "m.srli">;
defm PSRLI : PShift_ri<0b00000, 0b101, "p.srli">;

defm GSRAI : GShift_ri<0b01000, 0b101, "g.srai">;
defm MSRAI : MShift_ri<0b01000, 0b101, "m.srai">;
defm PSRAI : PShift_ri<0b01000, 0b101, "p.srai">;

def MADD  : MALU_rr<0b0000000, 0b000, "m.add", /*Commutable*/1>,
            Sched<[WriteIALU, ReadIALU, ReadIALU]>;
def PADD  : PALU_rr<0b0000000, 0b000, "p.add", /*Commutable*/1>,
            Sched<[WriteIALU, ReadIALU, ReadIALU]>;

def : AMiQualifiedAuto<"MADD">;
def : AMiQualifiedAuto<"PADD">;

let hasSideEffects = 0, mayLoad = 0, mayStore = 0 in
def MARK : RVInstI<0b000, OPC_MARK, (outs),
                   (ins simm12:$imm12),
                   "mark", "${imm12}">, Sched<[]> {
  let rs1 = 0;
  let rd = 0;
}

// Always persistent instructions
def : AMiAlwaysPersistent<"SB">;
def : AMiAlwaysPersistent<"SH">;
def : AMiAlwaysPersistent<"SW">;

//===----------------------------------------------------------------------===//
// Non-standard extensions
//===----------------------------------------------------------------------===//

def : InstAlias<"emm $offset", (ABEQ X0, X0, simm13_lsb0:$offset)>;
