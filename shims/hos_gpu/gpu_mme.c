/*
 * The Macro Method Expander (MME) of the 3D engine. Clients upload small
 * programs at start-up (methods 0x45-0x48) and call them with methods
 * 0xE00 + 2n (first parameter) and 0xE01 + 2n (the rest); a macro reads
 * parameters and the engine's registers and emits method writes. deko3d
 * clears through one, NVN draws through many, so the 3D engine is not
 * usable without this.
 *
 * Instruction set, as documented by envytools ("Fermi macro processor"):
 *
 *   [2:0]   operation: 0 ALU, 1 add immediate, 2 extract-insert, 3 extract
 *           and shift left by immediate, 4 extract and shift left by
 *           register, 5 read register, 7 branch
 *   [6:4]   result assignment (see assign below); for branches [4] is the
 *           condition (0 branch if zero, 1 if not zero) and [5] cancels the
 *           delay slot
 *   [7]     exit after the delay slot
 *   [10:8]  destination register, [13:11] source A, [16:14] source B
 *   [31:14] signed immediate (add immediate, read, branch)
 *   [21:17] ALU operation; or bitfield source bit, [26:22] size, [31:27]
 *           destination bit
 *
 * Registers r0..r7, r0 reads as zero; r1 starts with the first parameter.
 * The method register holds the address in [11:0] and an increment in
 * [17:12] applied after every send.
 */

#include "gpu_mme.h"

#include <string.h>

#include "common/hos_log.h"

#define MOD "mme"

#define MAX_STEPS 1000000 /* a runaway macro must not hang the emulator */

typedef struct mme_vm {
    uint32_t r[8];
    uint32_t method_addr;
    uint32_t carry;
    const uint32_t *params;
    uint32_t num_params;
    uint32_t next_param;
    mme_send_fn send;
    mme_read_fn read;
    void *user;
} mme_vm;

static uint32_t fetch(mme_vm *vm) {
    if (vm->next_param < vm->num_params) {
        return vm->params[vm->next_param++];
    }
    HOS_LOGW(MOD, "macro read past its %u parameters", vm->num_params);
    return 0;
}

static void send(mme_vm *vm, uint32_t value) {
    vm->send(vm->user, vm->method_addr & 0xFFF, value);
    uint32_t inc = (vm->method_addr >> 12) & 0x3F;
    vm->method_addr = (vm->method_addr & ~0xFFFu) | ((vm->method_addr + inc) & 0xFFF);
}

static void set_reg(mme_vm *vm, uint32_t reg, uint32_t v) {
    if (reg) {
        vm->r[reg] = v;
    }
}

static void assign(mme_vm *vm, uint32_t op, uint32_t dst, uint32_t result) {
    switch (op) {
    case 0: /* IgnoreAndFetch */
        set_reg(vm, dst, fetch(vm));
        break;
    case 1: /* Move */
        set_reg(vm, dst, result);
        break;
    case 2: /* MoveAndSetMethod */
        set_reg(vm, dst, result);
        vm->method_addr = result;
        break;
    case 3: /* FetchAndSend */
        set_reg(vm, dst, fetch(vm));
        send(vm, result);
        break;
    case 4: /* MoveAndSend */
        set_reg(vm, dst, result);
        send(vm, result);
        break;
    case 5: /* FetchAndSetMethod */
        set_reg(vm, dst, fetch(vm));
        vm->method_addr = result;
        break;
    case 6: /* MoveAndSetMethodFetchAndSend */
        set_reg(vm, dst, result);
        vm->method_addr = result;
        send(vm, fetch(vm));
        break;
    case 7: /* MoveAndSetMethodSend */
        set_reg(vm, dst, result);
        vm->method_addr = result;
        send(vm, (result >> 12) & 0x3F);
        break;
    default:
        break;
    }
}

static uint32_t alu(mme_vm *vm, uint32_t op, uint32_t a, uint32_t b) {
    uint64_t t;
    switch (op) {
    case 0: /* add */
        t = (uint64_t)a + b;
        vm->carry = (uint32_t)(t >> 32);
        return (uint32_t)t;
    case 1: /* add with carry */
        t = (uint64_t)a + b + vm->carry;
        vm->carry = (uint32_t)(t >> 32);
        return (uint32_t)t;
    case 2: /* subtract */
        t = (uint64_t)a - b;
        vm->carry = a >= b;
        return (uint32_t)t;
    case 3: /* subtract with borrow */
        t = (uint64_t)a - b - (vm->carry ? 0 : 1);
        vm->carry = (uint64_t)a >= (uint64_t)b + (vm->carry ? 0 : 1);
        return (uint32_t)t;
    case 8:
        return a ^ b;
    case 9:
        return a | b;
    case 10:
        return a & b;
    case 11:
        return a & ~b;
    case 12:
        return ~(a & b);
    default:
        HOS_LOGW(MOD, "unknown ALU operation %u", op);
        return 0;
    }
}

int mme_execute(const uint32_t *code, uint32_t code_words, uint32_t start, const uint32_t *params,
                uint32_t num_params, mme_send_fn send_fn, mme_read_fn read_fn, void *user) {
    mme_vm vm;
    memset(&vm, 0, sizeof(vm));
    vm.params = params;
    vm.num_params = num_params;
    vm.send = send_fn;
    vm.read = read_fn;
    vm.user = user;
    vm.r[1] = fetch(&vm);

    uint32_t pc = start;
    int64_t pending_jump = -1; /* target of a taken branch, after its delay slot */
    int exiting = 0;           /* the exit bit was seen; stop after the delay slot */
    for (uint32_t step = 0; step < MAX_STEPS; step++) {
        if (pc >= code_words) {
            HOS_LOGW(MOD, "macro ran off its code at %u", pc);
            return -1;
        }
        uint32_t insn = code[pc];
        uint32_t op = insn & 7, aop = (insn >> 4) & 7;
        uint32_t dst = (insn >> 8) & 7;
        uint32_t a = vm.r[(insn >> 11) & 7], b = vm.r[(insn >> 14) & 7];
        int32_t imm = (int32_t)insn >> 14;
        uint32_t next = pc + 1;
        int in_delay_slot = pending_jump >= 0 || exiting;

        if (op == 7) { /* branch */
            int if_nonzero = (insn >> 4) & 1, annul = (insn >> 5) & 1;
            int taken = if_nonzero ? a != 0 : a == 0;
            if (taken) {
                if (annul) {
                    next = (uint32_t)((int32_t)pc + imm);
                } else {
                    pending_jump = (int32_t)pc + imm;
                    pc = next;
                    continue;
                }
            }
        } else {
            uint32_t result = 0;
            uint32_t src_bit = (insn >> 17) & 0x1F, size = (insn >> 22) & 0x1F;
            uint32_t dst_bit = (insn >> 27) & 0x1F;
            uint32_t mask = size >= 32 ? 0xFFFFFFFFu : ((1u << size) - 1);
            switch (op) {
            case 0:
                result = alu(&vm, (insn >> 17) & 0x1F, a, b);
                break;
            case 1:
                result = a + (uint32_t)imm;
                break;
            case 2: /* extract-insert: b's field into a */
                result = (a & ~(mask << dst_bit)) | (((b >> src_bit) & mask) << dst_bit);
                break;
            case 3: /* extract by register a, shift left by immediate */
                result = ((b >> (a & 31)) & mask) << dst_bit;
                break;
            case 4: /* extract by immediate, shift left by register a */
                result = ((b >> src_bit) & mask) << (a & 31);
                break;
            case 5: /* read engine register */
                result = vm.read(vm.user, (b + (uint32_t)imm) & 0xFFF);
                break;
            default:
                HOS_LOGW(MOD, "unknown macro operation %u at %u", op, pc);
                return -1;
            }
            assign(&vm, aop, dst, result);
        }

        if (in_delay_slot) {
            if (exiting) {
                return 0;
            }
            next = (uint32_t)pending_jump;
            pending_jump = -1;
        }
        if ((insn >> 7) & 1) {
            exiting = 1;
        }
        pc = next;
    }
    HOS_LOGW(MOD, "macro did not finish in %u steps", MAX_STEPS);
    return -1;
}
