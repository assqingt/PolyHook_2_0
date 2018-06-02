//
// Created by steve on 7/5/17.
//
#include "headers/CapstoneDisassembler.hpp"

std::vector<PLH::Instruction>
PLH::CapstoneDisassembler::disassemble(uint64_t firstInstruction, uint64_t start, uint64_t End) {
    cs_insn* InsInfo = cs_malloc(m_capHandle);
    std::vector<PLH::Instruction> InsVec;

    uint64_t Size = End - start;
	while (cs_disasm_iter(m_capHandle, (const uint8_t**)&start, (size_t*)&Size, &firstInstruction, InsInfo)) {
		//Set later by 'SetDisplacementFields'
		PLH::Instruction::Displacement displacement;
		displacement.Absolute = 0;

		auto Inst = PLH::Instruction(InsInfo->address,
			displacement,
			0,
			false,
			InsInfo->bytes,
			InsInfo->size,
			InsInfo->mnemonic,
			InsInfo->op_str);

		setDisplacementFields(Inst, InsInfo);
		InsVec.push_back(Inst);

		// update jump map if the instruction is jump/call
		if (Inst.hasDisplacement()) {
			// search back, check if new instruction points to older ones (one to one)
			auto destInst = std::find_if(InsVec.begin(), InsVec.end(), [=](const PLH::Instruction& oldIns) {
				return oldIns.getAddress() == Inst.getDestination();
			});

			if (destInst != InsVec.end()) {
				updateBranchMap(destInst->getAddress(), Inst);
			}
		}

		// search forward, check if old instructions now point to new one (many to one possible)
		for (const PLH::Instruction& oldInst : InsVec) {
			if (oldInst.hasDisplacement() && oldInst.getDestination() == Inst.getAddress()) {
				updateBranchMap(Inst.getAddress(), oldInst);
			}
		}
	}
    cs_free(InsInfo, 1);
    return InsVec;
}

/**Write the raw bytes of the given instruction into the memory specified by the
 * instruction's address. If the address value of the instruction has been changed
 * since the time it was decoded this will copy the instruction to a new memory address.
 * This will not automatically do any code relocation, all relocation logic should
 * first modify the byte array, and then call write encoding, proper order to relocate
 * an instruction should be disasm instructions -> set relative/absolute displacement() ->
 * writeEncoding(). It is done this way so that these operations can be made transactional**/
void PLH::CapstoneDisassembler::writeEncoding(const PLH::Instruction& instruction) const {
    memcpy((void*)instruction.getAddress(), &instruction.getBytes()[0], instruction.size());
}

/**If an instruction is a jmp/call variant type this will set it's displacement fields to the
 * appropriate values. All other types of instructions are ignored as no-op. More specifically
 * this determines if an instruction is a jmp/call variant, and then further if it is is jumping via
 * memory or immediate, and then finally if that mem/imm is encoded via a displacement relative to
 * the instruction pointer, or directly to an absolute address**/
void PLH::CapstoneDisassembler::setDisplacementFields(PLH::Instruction& inst, const cs_insn* capInst) const {
    cs_x86 x86 = capInst->detail->x86;

    for (uint_fast32_t j = 0; j < x86.op_count; j++) {
        cs_x86_op op = x86.operands[j];
        if (op.type == X86_OP_MEM) {
            //Are we relative to instruction pointer?
            //mem are types like jmp [rip + 0x4] where location is dereference-d
            if (op.mem.base != getIpReg())
                continue;

            const uint8_t Offset = x86.encoding.disp_offset;
            const uint8_t Size   = x86.encoding.disp_size;

			// it's relative, set immDest to max to trigger later check
            copyDispSX(inst, Offset, Size, std::numeric_limits<int64_t>::max());
        } else if (op.type == X86_OP_IMM) {
            //IMM types are like call 0xdeadbeef where they jmp straight to some location
            if (!hasGroup(capInst, x86_insn_group::X86_GRP_JUMP) &&
                !hasGroup(capInst, x86_insn_group::X86_GRP_CALL))
                continue;

            const uint8_t Offset = x86.encoding.imm_offset;
            const uint8_t Size   = x86.encoding.imm_size;
            copyDispSX(inst, Offset, Size, op.imm);
        }
    }
}

/**Copies the displacement bytes from memory, and sign extends these values if necessary**/
void PLH::CapstoneDisassembler::copyDispSX(PLH::Instruction& inst,
                                                   const uint8_t offset,
                                                   const uint8_t size,
                                                   const int64_t immDestination) const {
    /* Sign extension necessary because we are storing numbers (possibly) smaller than int64_t that may be negative.
     * If we did not do this, then the sign bit would be in the incorrect place for an int64_t.
     * 1 << (Size*8-1) dynamically calculates the position of the sign bit (furthest left) (our byte mask)
     * the Size*8 gives us the size in bits, i do -1 because zero based. Then left shift to set that bit to one.
     * Then & that with the calculated mask to check if the sign bit is set in the retrieved displacement,
     * the result will be positive if sign bit is set (negative displacement)
     * and 0 when sign bit not set (positive displacement)*/
    int64_t displacement = 0;
    memcpy(&displacement, &inst.getBytes()[offset], size);

    uint64_t mask = (1U << (size * 8 - 1));
    if (displacement & (1U << (size * 8 - 1))) {
        /* sign extend if negative, requires that bits above Size*8 are zero,
         * if bits are not zero use x = x & ((1U << b) - 1) where x is a temp for displacement
         * and b is Size*8*/
        displacement = (displacement ^ mask) -
                       mask; //xor clears sign bit, subtraction makes number negative again but in the int64 range
    }

    inst.setDisplacementOffset(offset);

    /* When the retrieved displacement is < immDestination we know that the base address is included
     * in the destinations calculation. By definition this means it is relative. Otherwise it is absolute*/
    if (displacement < immDestination) {
        if (immDestination != std::numeric_limits<int64_t>::max())
            assert(displacement + inst.getAddress() + inst.size() == immDestination);
        inst.setRelativeDisplacement(displacement);
    } else {
        assert(((uint64_t)displacement) == ((uint64_t)immDestination));
        inst.setAbsoluteDisplacement((uint64_t)displacement);
    }
}

bool PLH::CapstoneDisassembler::isConditionalJump(const PLH::Instruction& instruction) const {
    //http://unixwiz.net/techtips/x86-jumps.html
    if (instruction.size() < 1)
        return false;

    std::vector<uint8_t> bytes = instruction.getBytes();
    if (bytes[0] == 0x0F && instruction.size() > 1) {
        if (bytes[1] >= 0x80 && bytes[1] <= 0x8F)
            return true;
    }

    if (bytes[0] >= 0x70 && bytes[0] <= 0x7F)
        return true;

    if (bytes[0] == 0xE3)
        return true;

    return false;
}