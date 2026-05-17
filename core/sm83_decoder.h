#ifndef SM83_DECODER_H
#define SM83_DECODER_H

#include "gb_types.h"

/* Static decode info: bytes including opcode (1..3), base cycles when no branch
   is taken. For conditional branches, cycles_taken holds the additional cycles
   when the branch is taken. */
typedef struct {
    u8 length;
    u8 cycles_notaken;   /* base cycles */
    u8 cycles_taken;     /* total cycles when branch taken (>= cycles_notaken) */
    u8 flags;            /* see SM83_OP_FLAG_* */
} sm83_op_info;

#define SM83_OP_FLAG_BRANCH     0x01u  /* changes PC besides falling through */
#define SM83_OP_FLAG_CONDITIONAL 0x02u
#define SM83_OP_FLAG_CALL       0x04u
#define SM83_OP_FLAG_RET        0x08u
#define SM83_OP_FLAG_HALT       0x10u
#define SM83_OP_FLAG_STOP       0x20u
#define SM83_OP_FLAG_EI         0x40u
#define SM83_OP_FLAG_DI         0x80u

/* Returns the info for a base opcode. CB-prefixed (0xCB nn) all have length 2,
   cycles 8 (16 if mem operand). Caller adjusts. */
const sm83_op_info *sm83_decode(u8 opcode);

/* True if `opcode` terminates a basic block (branch/call/ret/halt/stop). */
bool sm83_terminates_block(u8 opcode);

#endif
