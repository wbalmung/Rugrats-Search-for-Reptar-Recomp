"""R3000A instruction decoder (PS1 subset incl. COP0/COP2)."""

REG = ["zero", "at", "v0", "v1", "a0", "a1", "a2", "a3",
       "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
       "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7",
       "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra"]


class Ins:
    __slots__ = ("addr", "word", "op", "rs", "rt", "rd", "sa", "imm", "simm", "target", "name")

    def __init__(self, addr, word):
        self.addr = addr
        self.word = word
        self.op = word >> 26
        self.rs = (word >> 21) & 31
        self.rt = (word >> 16) & 31
        self.rd = (word >> 11) & 31
        self.sa = (word >> 6) & 31
        self.imm = word & 0xFFFF
        self.simm = self.imm - 0x10000 if self.imm & 0x8000 else self.imm
        self.target = ((addr + 4) & 0xF0000000) | ((word & 0x3FFFFFF) << 2)
        self.name = decode_name(word)

    # control-flow classification
    def is_branch(self):  # conditional/unconditional PC-relative, has delay slot
        return self.name in ("beq", "bne", "blez", "bgtz", "bltz", "bgez", "bltzal", "bgezal")

    def branch_target(self):
        return (self.addr + 4 + (self.simm << 2)) & 0xFFFFFFFF

    def has_delay(self):
        return self.is_branch() or self.name in ("j", "jal", "jr", "jalr")

    def __repr__(self):
        return f"{self.addr:08x}: {self.word:08x} {self.name}"


SPECIAL = {0: "sll", 2: "srl", 3: "sra", 4: "sllv", 6: "srlv", 7: "srav", 8: "jr", 9: "jalr",
           12: "syscall", 13: "break", 16: "mfhi", 17: "mthi", 18: "mflo", 19: "mtlo",
           24: "mult", 25: "multu", 26: "div", 27: "divu", 32: "add", 33: "addu", 34: "sub",
           35: "subu", 36: "and", 37: "or", 38: "xor", 39: "nor", 42: "slt", 43: "sltu"}
PRIMARY = {2: "j", 3: "jal", 4: "beq", 5: "bne", 6: "blez", 7: "bgtz", 8: "addi", 9: "addiu",
           10: "slti", 11: "sltiu", 12: "andi", 13: "ori", 14: "xori", 15: "lui",
           32: "lb", 33: "lh", 34: "lwl", 35: "lw", 36: "lbu", 37: "lhu", 38: "lwr",
           40: "sb", 41: "sh", 42: "swl", 43: "sw", 46: "swr", 50: "lwc2", 58: "swc2"}


def decode_name(w):
    op = w >> 26
    if op == 0:
        return SPECIAL.get(w & 63, "invalid")
    if op == 1:
        rt = (w >> 16) & 31
        return {0: "bltz", 1: "bgez", 16: "bltzal", 17: "bgezal"}.get(rt, "invalid")
    if op == 16 or op == 18:
        rs = (w >> 21) & 31
        cop = "0" if op == 16 else "2"
        if rs & 16:
            return "rfe" if op == 16 and (w & 63) == 16 else ("cop2" if op == 18 else "cop0op")
        return {0: "mfc" + cop, 2: "cfc" + cop, 4: "mtc" + cop, 6: "ctc" + cop}.get(rs, "invalid")
    return PRIMARY.get(op, "invalid")
