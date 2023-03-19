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
// AMi Qualifier Tables
//===----------------------------------------------------------------------===//

class AMiClass;

def Mimicable: AMiClass;
def Activatable: AMiClass;
def AlwaysPersistent: AMiClass;

class QualifiedInst<int quallen = 1> {
  string AMiFromInst = !substr(NAME, quallen);
  string AMiQualInst = NAME;
  string AMiQualifier = !substr(NAME, 0, quallen);
  AMiClass AMiClass = Mimicable;
}

class AMiAlwaysPersistent : QualifiedInst<1> {
  let AMiFromInst = !substr(NAME, 1);
  let AMiQualInst = !substr(NAME, 1);
  let AMiQualifier = "p";
  let AMiClass = AlwaysPersistent;
}

//===----------------------------------------------------------------------===//
// AMi Qualified Instruction Template Multiclasses
//===----------------------------------------------------------------------===//

multiclass AMiBranchCC_rri<bits<3> funct3, string opcodestr> {
let AMiClass = Activatable, hasSideEffects = 1, isBranch = 0 in {
  let Opcode = OPC_ABRANCH.Value in
  def A#NAME : BranchCC_rri<funct3, "a."#opcodestr>, QualifiedInst;
  let Opcode = OPC_CTBRANCH.Value in
  def CT#NAME : BranchCC_rri<funct3, "ct."#opcodestr>, QualifiedInst<2>;
}
}

multiclass AMiLoad_ri<bits<3> funct3, string opcodestr> {
let AMiClass = Mimicable in {
  let Opcode = OPC_GLOAD.Value in
  def G#NAME : Load_ri<funct3, "g."#opcodestr>, QualifiedInst;
  let Opcode = OPC_MLOAD.Value in
  def M#NAME : Load_ri<funct3, "m."#opcodestr>, QualifiedInst;
  let Opcode = OPC_PLOAD.Value in
  def P#NAME : Load_ri<funct3, "p."#opcodestr>, QualifiedInst;
}
}

multiclass AMiALU_ri<bits<3> funct3, string opcodestr> {
let AMiClass = Mimicable in {
  let Opcode = OPC_OP_GIMM.Value in
  def G#NAME : ALU_ri<funct3, "g."#opcodestr>, QualifiedInst;
  let Opcode = OPC_OP_MIMM.Value in
  def M#NAME : ALU_ri<funct3, "m."#opcodestr>, QualifiedInst;
  let Opcode = OPC_OP_PIMM.Value in
  def P#NAME : ALU_ri<funct3, "p."#opcodestr>, QualifiedInst;
}
}

multiclass AMiShift_ri<bits<5> imm11_7, bits<3> funct3, string opcodestr> {
let AMiClass = Mimicable in {
  let Opcode = OPC_OP_GIMM.Value in
  def G#NAME : Shift_ri<imm11_7, funct3, "g."#opcodestr>, QualifiedInst;
  let Opcode = OPC_OP_MIMM.Value in
  def M#NAME : Shift_ri<imm11_7, funct3, "m."#opcodestr>, QualifiedInst;
  let Opcode = OPC_OP_PIMM.Value in
  def P#NAME : Shift_ri<imm11_7, funct3, "p."#opcodestr>, QualifiedInst;
}
}

multiclass AMiALU_rr<bits<7> funct7, bits<3> funct3, string opcodestr, bit Commutable = 0> {
let AMiClass = Mimicable in {
  let Opcode = OPC_OPM.Value in
  def M#NAME : ALU_rr<funct7, funct3, "m."#opcodestr, Commutable>, QualifiedInst;
  let Opcode = OPC_OPP.Value in
  def P#NAME : ALU_rr<funct7, funct3, "p."#opcodestr, Commutable>, QualifiedInst;
}
}

//===----------------------------------------------------------------------===//
// Instructions
//===----------------------------------------------------------------------===//

let hasSideEffects = 0, mayLoad = 0, mayStore = 0 in {
let isReMaterializable = 1, isAsCheapAsAMove = 1 in
def PLUI : RVInstU<OPC_PLUI, (outs GPR:$rd), (ins uimm20_lui:$imm20),
                  "p.lui", "$rd, $imm20">, Sched<[WriteIALU]>, QualifiedInst;

let AMiClass = Activatable in {
let isCall = 1 in
def AJAL : RVInstJ<OPC_AJAL, (outs GPR:$rd), (ins simm21_lsb0_jal:$imm20),
                  "a.jal", "$rd, $imm20">, Sched<[WriteJal]>, QualifiedInst;

let isCall = 1 in
def AJALR : RVInstI<0b000, OPC_AJALR, (outs GPR:$rd),
                   (ins GPR:$rs1, simm12:$imm12),
                   "a.jalr", "$rd, ${imm12}(${rs1})">,
           Sched<[WriteJalr, ReadJalr]>, QualifiedInst;
}
}

defm BEQ  : AMiBranchCC_rri<0b000, "beq">;
defm BNE  : AMiBranchCC_rri<0b001, "bne">;
defm BLT  : AMiBranchCC_rri<0b100, "blt">;
defm BGE  : AMiBranchCC_rri<0b101, "bge">;
defm BLTU : AMiBranchCC_rri<0b110, "bltu">;
defm BGEU : AMiBranchCC_rri<0b111, "bgeu">;

defm LB : AMiLoad_ri<0b000, "lb">, Sched<[WriteLDB, ReadMemBase]>;
defm LH : AMiLoad_ri<0b001, "lh">, Sched<[WriteLDH, ReadMemBase]>;
defm LW : AMiLoad_ri<0b010, "lw">, Sched<[WriteLDW, ReadMemBase]>;
defm LBU : AMiLoad_ri<0b100, "lbu">, Sched<[WriteLDB, ReadMemBase]>;
defm LHU : AMiLoad_ri<0b101, "lhu">, Sched<[WriteLDH, ReadMemBase]>;

let isReMaterializable = 1, isAsCheapAsAMove = 1 in
defm ADDI  : AMiALU_ri<0b000, "addi">;

defm SLTI  : AMiALU_ri<0b010, "slti">;
defm SLTIU : AMiALU_ri<0b011, "sltiu">;

let isReMaterializable = 1, isAsCheapAsAMove = 1 in {
defm XORI  : AMiALU_ri<0b100, "xori">;
defm ORI   : AMiALU_ri<0b110, "ori">;
}

defm ANDI  : AMiALU_ri<0b111, "andi">;

defm SLLI : AMiShift_ri<0b00000, 0b001, "slli">;
defm SRLI : AMiShift_ri<0b00000, 0b101, "srli">;
defm SRAI : AMiShift_ri<0b01000, 0b101, "srai">;

defm ADD  : AMiALU_rr<0b0000000, 0b000, "add", /*Commutable*/1>,
           Sched<[WriteIALU, ReadIALU, ReadIALU]>;
defm SUB  : AMiALU_rr<0b0100000, 0b000, "sub">,
           Sched<[WriteIALU, ReadIALU, ReadIALU]>;
defm SLL  : AMiALU_rr<0b0000000, 0b001, "sll">,
           Sched<[WriteShiftReg, ReadShiftReg, ReadShiftReg]>;
defm SLT  : AMiALU_rr<0b0000000, 0b010, "slt">,
           Sched<[WriteIALU, ReadIALU, ReadIALU]>;
defm SLTU : AMiALU_rr<0b0000000, 0b011, "sltu">,
           Sched<[WriteIALU, ReadIALU, ReadIALU]>;
defm XOR  : AMiALU_rr<0b0000000, 0b100, "xor", /*Commutable*/1>,
           Sched<[WriteIALU, ReadIALU, ReadIALU]>;
defm SRL  : AMiALU_rr<0b0000000, 0b101, "srl">,
           Sched<[WriteShiftReg, ReadShiftReg, ReadShiftReg]>;
defm SRA  : AMiALU_rr<0b0100000, 0b101, "sra">,
           Sched<[WriteShiftReg, ReadShiftReg, ReadShiftReg]>;
defm OR   : AMiALU_rr<0b0000000, 0b110, "or", /*Commutable*/1>,
           Sched<[WriteIALU, ReadIALU, ReadIALU]>;
defm AND  : AMiALU_rr<0b0000000, 0b111, "and", /*Commutable*/1>,
           Sched<[WriteIALU, ReadIALU, ReadIALU]>;

let hasSideEffects = 0, mayLoad = 0, mayStore = 0 in
def MARK : RVInstI<0b000, OPC_MARK, (outs),
                   (ins simm12:$imm12),
                   "mark", "${imm12}">, Sched<[]> {
  let rs1 = 0;
  let rd = 0;
}

let isBarrier = 1, isBranch = 0, isTerminator = 1, hasSideEffects = 1 in
def APseudoBR : Pseudo<(outs), (ins simm21_lsb0_jal:$imm20), [(br bb:$imm20)]>,
                PseudoInstExpansion<(AJAL X0, simm21_lsb0_jal:$imm20)>, QualifiedInst;

let isBarrier = 1, isBranch = 0, isIndirectBranch = 0, isTerminator = 1, hasSideEffects = 1 in
def APseudoBRIND : Pseudo<(outs), (ins GPRJALR:$rs1, simm12:$imm12), []>,
                   PseudoInstExpansion<(AJALR X0, GPR:$rs1, simm12:$imm12)>, QualifiedInst;

def : Pat<(brind GPRJALR:$rs1), (APseudoBRIND GPRJALR:$rs1, 0)>;
def : Pat<(brind (add GPRJALR:$rs1, simm12:$imm12)),
          (APseudoBRIND GPRJALR:$rs1, simm12:$imm12)>;

// Always persistent instructions
def PSB : AMiAlwaysPersistent;
def PSH : AMiAlwaysPersistent;
def PSW : AMiAlwaysPersistent;

//===----------------------------------------------------------------------===//
// Non-standard extensions
//===----------------------------------------------------------------------===//

def : InstAlias<"emm $offset", (ABEQ X0, X0, simm13_lsb0:$offset)>;
