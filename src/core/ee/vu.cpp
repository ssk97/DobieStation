#include <cmath>
#include "../logger.hpp"
#include <cstdlib>
#include "vu.hpp"
#include "vu_interpreter.hpp"

#include "../gif.hpp"

#define _x(f) f&8
#define _y(f) f&4
#define _z(f) f&2
#define _w(f) f&1

#define printf(fmt, ...)(0)

VectorUnit::VectorUnit(int id) : id(id), gif(nullptr)
{
    gpr[0].f[0] = 0.0;
    gpr[0].f[1] = 0.0;
    gpr[0].f[2] = 0.0;
    gpr[0].f[3] = 1.0;
    int_gpr[0] = 0;

    VIF_TOP = nullptr;
    VIF_ITOP = nullptr;

    MAC_flags = &MAC_pipeline[3];
}

void VectorUnit::reset()
{
    status = 0;
    clip_flags = 0;
    PC = 0;
    running = false;
    finish_on = false;
    branch_on = false;
    delay_slot = 0;
    transferring_GIF = false;
    new_MAC_flags = 0;
}

void VectorUnit::set_TOP_regs(uint16_t *TOP, uint16_t *ITOP)
{
    VIF_TOP = TOP;
    VIF_ITOP = ITOP;
}

void VectorUnit::set_GIF(GraphicsInterface *gif)
{
    this->gif = gif;
}

void VectorUnit::run(int cycles)
{
    int cycles_to_run = cycles;
    while (running && cycles_to_run)
    {
        update_mac_pipeline();
        uint32_t upper_instr = *(uint32_t*)&instr_mem[PC + 4];
        uint32_t lower_instr = *(uint32_t*)&instr_mem[PC];
        Logger::log(Logger::OTHER, "[$%08X] $%08X:$%08X\n", PC, upper_instr, lower_instr);
        VU_Interpreter::interpret(*this, upper_instr, lower_instr);
        PC += 8;
        if (branch_on)
        {
            if (!delay_slot)
            {
                PC = new_PC;
                branch_on = false;
            }
            else
                delay_slot--;
        }
        if (finish_on)
        {
            if (!delay_slot)
            {
                running = false;
                finish_on = false;
            }
            else
                delay_slot--;
        }
        cycles_to_run--;
    }

    int GIF_cycles = cycles;
    while (transferring_GIF && GIF_cycles)
    {
        uint128_t quad = read_data<uint128_t>(GIF_addr);
        if (gif->send_PATH1(quad))
        {
            gif->deactivate_PATH(1);
            transferring_GIF = false;
        }
        GIF_addr += 16;
        GIF_cycles--;
    }
}

void VectorUnit::mscal(uint32_t addr)
{
    Logger::log(Logger::VU, "Starting execution at $%08X!\n", addr);
    running = true;
    PC = addr;
}

void VectorUnit::end_execution()
{
    delay_slot = 1;
    finish_on = true;
}

void VectorUnit::update_mac_flags(float value, int index)
{
    int flag_id = 3 - index;
    new_MAC_flags &= ~(0x1111 << flag_id);

    //Zero flag
    new_MAC_flags |= (value == 0.0) << flag_id;

    //Sign flag
    new_MAC_flags |= (value < 0) << (flag_id + 4);
}

void VectorUnit::clear_mac_flags(int index)
{
    new_MAC_flags &= ~(0x1111 << (3 - index));
}

void VectorUnit::update_mac_pipeline()
{
    MAC_pipeline[3] = MAC_pipeline[2];
    MAC_pipeline[2] = MAC_pipeline[1];
    MAC_pipeline[1] = MAC_pipeline[0];
    MAC_pipeline[0] = new_MAC_flags;
}

float VectorUnit::convert(uint32_t value)
{
    switch(value & 0x7f800000)
    {
        case 0x0:
            value &= 0x80000000;
            return *(float*)&value;
        case 0x7f800000:
        {
            uint32_t result = (value & 0x80000000)|0x7f7fffff;
            return *(float*)&result;
        }
    }
    return *(float*)&value;
}

void VectorUnit::print_vectors(uint8_t a, uint8_t b)
{
    Logger::log(Logger::OTHER, "A: ");
    for (int i = 0; i < 4; i++)
        Logger::log(Logger::OTHER, "%f ", gpr[a].f[i]);
    Logger::log(Logger::OTHER, "\nB: ");
    for (int i = 0; i < 4; i++)
        Logger::log(Logger::OTHER, "%f ", gpr[b].f[i]);
    Logger::log(Logger::OTHER, "\n");
}

/**
 * Code taken from PCSX2 and adapted to DobieStation
 * https://github.com/PCSX2/pcsx2/blob/1292cd505efe7c68ab87880b4fd6809a96da703c/pcsx2/VUops.cpp#L1795
 */
void VectorUnit::advance_r()
{
    int x = (R.u >> 4) & 1;
    int y = (R.u >> 22) & 1;
    R.u <<= 1;
    R.u ^= x ^ y;
    R.u = (R.u & 0x7FFFFF) | 0x3F800000;
}

uint32_t VectorUnit::qmfc2(int id, int field)
{
    return gpr[id].u[field];
}

uint32_t VectorUnit::cfc(int index)
{
    if (index < 16)
        return int_gpr[index];
    switch (index)
    {
        case 16:
            return status;
        case 18:
            return clip_flags;
        case 20:
            return R.u & 0x7FFFFF;
        case 21:
            return I.u;
        case 22:
            return Q.u;
        default:
            Logger::log(Logger::COP2, "Unrecognized cfc2 from reg %d\n", index);
    }
    return 0;
}

void VectorUnit::ctc(int index, uint32_t value)
{
    if (index < 16)
    {
        Logger::log(Logger::COP2, "Set vi%d to $%04X\n", index, value);
        set_int(index, value);
        return;
    }
    switch (index)
    {
        case 20:
            R.u = value;
            break;
        case 21:
            I.u = value;
            Logger::log(Logger::VU, "I = %f\n", I.f);
            break;
        case 22:
            Q.u = value;
            break;
        default:
            Logger::log(Logger::COP2, "Unrecognized ctc2 of $%08X to reg %d\n", value, index);
    }
}

void VectorUnit::branch(bool condition, int32_t imm)
{
    if (condition)
    {
        branch_on = true;
        delay_slot = 1;
        new_PC = PC + imm + 8;
    }
}

void VectorUnit::jp(uint16_t addr)
{
    new_PC = addr;
    branch_on = true;
    delay_slot = 1;
}

void VectorUnit::abs(uint8_t field, uint8_t dest, uint8_t source)
{
    Logger::log(Logger::VU, "ABS: ");
    for (int i = 0; i < 4; i++)
    {
        if (field & (1 << (3 - i)))
        {
            float result = fabs(convert(gpr[source].u[i]));
            set_gpr_f(dest, i, result);
            Logger::log(Logger::OTHER, "(%d)%f ", i, gpr[dest].f[i]);
        }
    }
    Logger::log(Logger::OTHER, "\n");
}

void VectorUnit::add(uint8_t field, uint8_t dest, uint8_t reg1, uint8_t reg2)
{
    Logger::log(Logger::VU, "ADD: ");
    for (int i = 0; i < 4; i++)
    {
        if (field & (1 << (3 - i)))
        {
            float result = convert(gpr[reg1].u[i]) + convert(gpr[reg2].u[i]);
            update_mac_flags(result, i);
            set_gpr_f(dest, i, result);
            Logger::log(Logger::OTHER, "(%d)%f ", i, gpr[dest].f[i]);
        }
        else
            clear_mac_flags(i);
    }
    Logger::log(Logger::OTHER, "\n");
}

void VectorUnit::adda(uint8_t field, uint8_t reg1, uint8_t reg2)
{
    Logger::log(Logger::VU, "ADDA: ");
    for (int i = 0; i < 4; i++)
    {
        if (field & (1 << (3 - i)))
        {
            ACC.f[i] = convert(gpr[reg1].u[i]) + convert(gpr[reg2].u[i]);
            update_mac_flags(ACC.f[i], i);
            Logger::log(Logger::OTHER, "(%d)%f ", i, ACC.f[i]);
        }
        else
            clear_mac_flags(i);
    }
    Logger::log(Logger::OTHER, "\n");
}

void VectorUnit::addabc(uint8_t bc, uint8_t field, uint8_t source, uint8_t bc_reg)
{
    Logger::log(Logger::VU, "ADDAbc: ");
    float op = convert(gpr[bc_reg].u[bc]);
    for (int i = 0; i < 4; i++)
    {
        if (field & (1 << (3 - i)))
        {
            ACC.f[i] = op + convert(gpr[source].u[i]);
            update_mac_flags(ACC.f[i], i);
            Logger::log(Logger::OTHER, "(%d)%f", i, ACC.f[i]);
        }
        else
            clear_mac_flags(i);
    }
    Logger::log(Logger::OTHER, "\n");
}

void VectorUnit::addbc(uint8_t bc, uint8_t field, uint8_t dest, uint8_t source, uint8_t bc_reg)
{
    Logger::log(Logger::VU, "ADDbc: ");
    float op = convert(gpr[bc_reg].u[bc]);
    for (int i = 0; i < 4; i++)
    {
        if (field & (1 << (3 - i)))
        {
            float temp = op + convert(gpr[source].u[i]);
            update_mac_flags(temp, i);
            set_gpr_f(dest, i, temp);
            Logger::log(Logger::OTHER, "(%d)%f ", i, gpr[dest].f[i]);
        }
        else
            clear_mac_flags(i);
    }
    Logger::log(Logger::OTHER, "\n");
}

void VectorUnit::addi(uint8_t field, uint8_t dest, uint8_t source)
{
    Logger::log(Logger::VU, "ADDi: ");
    float op = convert(I.u);
    for (int i = 0; i < 4; i++)
    {
        if (field & (1 << (3 - i)))
        {
            float temp = op + convert(gpr[source].u[i]);
            update_mac_flags(temp, i);
            set_gpr_f(dest, i, temp);
            Logger::log(Logger::OTHER, "(%d)%f ", i, gpr[dest].f[i]);
        }
        else
            clear_mac_flags(i);
    }
    Logger::log(Logger::OTHER, "\n");
}

void VectorUnit::addq(uint8_t field, uint8_t dest, uint8_t source)
{
    Logger::log(Logger::VU, "ADDq: ");
    float value = convert(Q.u);
    for (int i = 0; i < 4; i++)
    {
        if (field & (1 << (3 - i)))
        {
            float temp = value + convert(gpr[source].u[i]);
            update_mac_flags(temp, i);
            set_gpr_f(dest, i, temp);
            Logger::log(Logger::OTHER, "(%d)%f ", i, gpr[dest].f[i]);
        }
        else
            clear_mac_flags(i);
    }
    Logger::log(Logger::OTHER, "\n");
}

void VectorUnit::clip(uint8_t reg1, uint8_t reg2)
{
    Logger::log(Logger::VU, "CLIP\n");
    clip_flags <<= 6; //Move previous clipping judgments up

    //Compare x, y, z fields of FS with the w field of FT
    float value = fabs(convert(gpr[reg2].u[3]));

    float x = convert(gpr[reg1].u[0]);
    float y = convert(gpr[reg1].u[1]);
    float z = convert(gpr[reg1].u[2]);

    clip_flags |= (x > +value);
    clip_flags |= (x < -value) << 1;
    clip_flags |= (y > +value) << 2;
    clip_flags |= (y < -value) << 3;
    clip_flags |= (z > +value) << 4;
    clip_flags |= (z < -value) << 5;
    clip_flags &= 0xFFFFFF;
}

void VectorUnit::div(uint8_t ftf, uint8_t fsf, uint8_t reg1, uint8_t reg2)
{
    float num = convert(gpr[reg1].u[fsf]);
    float denom = convert(gpr[reg2].u[ftf]);
    status = (status & 0xFCF) | ((status & 0x30) << 6);
    if (denom == 0.0)
    {
        if (num == 0.0)
            status |= 0x10;
        else
            status |= 0x20;

        if ((gpr[reg1].u[fsf] & 0x80000000) != (gpr[reg2].u[ftf] & 0x80000000))
            Q.u = 0xFF7FFFFF;
        else
            Q.u = 0x7F7FFFFF;
    }
    else
    {
        Q.f = num / denom;
        Q.f = convert(Q.u);
    }
    Logger::log(Logger::VU, "DIV: %f\n", Q.f);
    Logger::log(Logger::OTHER, "Reg1: %f\n", num);
    Logger::log(Logger::OTHER, "Reg2: %f\n", denom);
}

void VectorUnit::eleng(uint8_t source)
{
    if (!id)
    {
        Logger::log(Logger::VU, "ERROR: ELENG called on VU0!\n");
        exit(1);
    }

    //P = sqrt(x^2 + y^2 + z^2)
    P.f = pow(convert(gpr[source].u[0]), 2) + pow(convert(gpr[source].u[1]), 2) + pow(convert(gpr[source].u[2]), 2);
    P.f = sqrt(P.f);

    Logger::log(Logger::VU, "ELENG: %f (%d)\n", P.f, source);
}

void VectorUnit::esqrt(uint8_t fsf, uint8_t source)
{
    if (!id)
    {
        Logger::log(Logger::VU, "ERROR: ESQRT called on VU0!\n");
        exit(1);
    }

    P.f = sqrt(fabs(convert(gpr[source].u[fsf])));

    Logger::log(Logger::VU, "ESQRT: %f (%d)\n", P.f, source);
}

void VectorUnit::fcand(uint32_t value)
{
    Logger::log(Logger::VU, "FCAND: $%08X\n", value);
    set_int(1, clip_flags & value);
}

void VectorUnit::fcset(uint32_t value)
{
    Logger::log(Logger::VU, "FCSET: $%08X\n", value);
    clip_flags = value;
}

void VectorUnit::fmand(uint8_t dest, uint8_t source)
{
    Logger::log(Logger::VU, "FMAND: $%04X\n", int_gpr[source]);
    set_int(dest, *MAC_flags & int_gpr[source]);
}

void VectorUnit::ftoi0(uint8_t field, uint8_t dest, uint8_t source)
{
    Logger::log(Logger::VU, "FTOI0: ");
    for (int i = 0; i < 4; i++)
    {
        if (field & (1 << (3 - i)))
        {
            gpr[dest].s[i] = (int32_t)convert(gpr[source].u[i]);
            Logger::log(Logger::OTHER, "(%d)$%08X ", i, gpr[dest].s[i]);
        }
    }
    Logger::log(Logger::OTHER, "\n");
}

void VectorUnit::ftoi4(uint8_t field, uint8_t dest, uint8_t source)
{
    Logger::log(Logger::VU, "FTOI4: ");
    for (int i = 0; i < 4; i++)
    {
        if (field & (1 << (3 - i)))
        {
            gpr[dest].s[i] = (int32_t)(convert(gpr[source].u[i]) * (1.0f / 0.0625f));
            Logger::log(Logger::OTHER, "(%d)$%08X ", i, gpr[dest].s[i]);
        }
    }
    Logger::log(Logger::OTHER, "\n");
}

void VectorUnit::ftoi12(uint8_t field, uint8_t dest, uint8_t source)
{
    Logger::log(Logger::VU, "FTOI12: ");
    for (int i = 0; i < 4; i++)
    {
        if (field & (1 << (3 - i)))
        {
            gpr[dest].s[i] = (int32_t)(convert(gpr[source].u[i]) * (1.0f / 0.000244140625f));
            Logger::log(Logger::OTHER, "(%d)$%08X ", i, gpr[dest].s[i]);
        }
    }
    Logger::log(Logger::OTHER, "\n");
}

void VectorUnit::ftoi15(uint8_t field, uint8_t dest, uint8_t source)
{
    Logger::log(Logger::VU, "FTOI15: ");
    for (int i = 0; i < 4; i++)
    {
        if (field & (1 << (3 - i)))
        {
            gpr[dest].s[i] = (int32_t)(convert(gpr[source].u[i]) * (1.0f / 0.000030517578125));
            Logger::log(Logger::OTHER, "(%d)$%08X ", i, gpr[dest].s[i]);
        }
    }
    Logger::log(Logger::OTHER, "\n");
}

void VectorUnit::iadd(uint8_t dest, uint8_t reg1, uint8_t reg2)
{
    set_int(dest, int_gpr[reg1] + int_gpr[reg2]);
    Logger::log(Logger::VU, "IADD: $%04X (%d, %d, %d)\n", int_gpr[dest], dest, reg1, reg2);
}

void VectorUnit::iaddi(uint8_t dest, uint8_t source, int8_t imm)
{
    set_int(dest, int_gpr[source] + imm);
    Logger::log(Logger::VU, "IADDI: $%04X (%d, %d, %d)\n", int_gpr[dest], dest, source, imm);
}

void VectorUnit::iaddiu(uint8_t dest, uint8_t source, uint16_t imm)
{
    set_int(dest, int_gpr[source] + imm);
    Logger::log(Logger::VU, "IADDIU: $%04X (%d, %d, $%04X)\n", int_gpr[dest], dest, source, imm);
}

void VectorUnit::iand(uint8_t dest, uint8_t reg1, uint8_t reg2)
{
    set_int(dest, int_gpr[reg1] & int_gpr[reg2]);
    Logger::log(Logger::VU, "IAND: $%04X (%d, %d, %d)\n", int_gpr[dest], dest, reg1, reg2);
}

void VectorUnit::ilw(uint8_t field, uint8_t dest, uint8_t base, int32_t offset)
{
    uint32_t addr = (int_gpr[base] << 4) + offset;
    uint128_t quad = read_data<uint128_t>(addr);
    Logger::log(Logger::VU, "ILW: $%08X ($%08X)\n", addr, offset);
    for (int i = 0; i < 4; i++)
    {
        if (field & (1 << (3 - i)))
        {
            Logger::log(Logger::OTHER, " $%04X ($%02X, %d, %d)", quad._u32[i] & 0xFFFF, field, dest, base);
            set_int(dest, quad._u32[i] & 0xFFFF);
            break;
        }
    }
    Logger::log(Logger::OTHER, "\n");
}

void VectorUnit::ilwr(uint8_t field, uint8_t dest, uint8_t base)
{
    uint32_t addr = int_gpr[base] << 4;
    uint128_t quad = read_data<uint128_t>(addr);
    Logger::log(Logger::VU, "ILWR: $%08X", addr);
    for (int i = 0; i < 4; i++)
    {
        if (field & (1 << (3 - i)))
        {
            Logger::log(Logger::OTHER, " $%04X ($%02X, %d, %d)", quad._u32[i] & 0xFFFF, field, dest, base);
            set_int(dest, quad._u32[i] & 0xFFFF);
            break;
        }
    }
    Logger::log(Logger::OTHER, "\n");
}

void VectorUnit::ior(uint8_t dest, uint8_t reg1, uint8_t reg2)
{
    set_int(dest, int_gpr[reg1] | int_gpr[reg2]);
    Logger::log(Logger::VU, "IOR: $%04X (%d, %d, %d)\n", int_gpr[dest], dest, reg1, reg2);
}

void VectorUnit::isub(uint8_t dest, uint8_t reg1, uint8_t reg2)
{
    set_int(dest, int_gpr[reg1] - int_gpr[reg2]);
    Logger::log(Logger::VU, "ISUB: $%04X (%d, %d, %d)\n", int_gpr[dest], dest, reg1, reg2);
}

void VectorUnit::isubiu(uint8_t dest, uint8_t source, uint16_t imm)
{
    set_int(dest, int_gpr[source] - imm);
    Logger::log(Logger::VU, "ISUBIU: $%04X (%d, %d, $%04X)\n", int_gpr[dest], dest, source, imm);
}

void VectorUnit::isw(uint8_t field, uint8_t source, uint8_t base, int32_t offset)
{
    uint32_t addr = (int_gpr[base] << 4) + offset;
    Logger::log(Logger::VU, "ISW: $%08X ($%08X)\n", addr, offset);
    for (int i = 0; i < 4; i++)
    {
        if (field & (1 << (3 - i)))
        {
            Logger::log(Logger::OTHER, "($%02X, %d, %d)\n", field, source, base);
            write_data<uint32_t>(addr + (i * 4), int_gpr[source]);
            break;
        }
    }
}

void VectorUnit::iswr(uint8_t field, uint8_t source, uint8_t base)
{
    uint32_t addr = int_gpr[base] << 4;
    Logger::log(Logger::VU, "ISWR to $%08X!\n", addr);
}

void VectorUnit::itof0(uint8_t field, uint8_t dest, uint8_t source)
{
    Logger::log(Logger::VU, "ITOF0: ");
    for (int i = 0; i < 4; i++)
    {
        if (field & (1 << (3 - i)))
        {
            set_gpr_f(dest, i, (float)gpr[source].s[i]);
            Logger::log(Logger::OTHER, "(%d)%f ", i, gpr[dest].f[i]);
        }
    }
    Logger::log(Logger::OTHER, "\n");
}

void VectorUnit::itof4(uint8_t field, uint8_t dest, uint8_t source)
{
    Logger::log(Logger::VU, "ITOF4: ");
    for (int i = 0; i < 4; i++)
    {
        if (field & (1 << (3 - i)))
        {
            gpr[dest].f[i] = (float)((float)gpr[source].s[i] * 0.0625f);
            Logger::log(Logger::OTHER, "(%d)%f ", i, gpr[dest].f[i]);
        }
    }
    Logger::log(Logger::OTHER, "\n");
}

void VectorUnit::itof12(uint8_t field, uint8_t dest, uint8_t source)
{
    Logger::log(Logger::VU, "ITOF12: ");
    for (int i = 0; i < 4; i++)
    {
        if (field & (1 << (3 - i)))
        {
            gpr[dest].f[i] = (float)((float)gpr[source].s[i] * 0.000244140625f);
            Logger::log(Logger::OTHER, "(%d)%f ", i, gpr[dest].f[i]);
        }
    }
    Logger::log(Logger::OTHER, "\n");
}

void VectorUnit::lq(uint8_t field, uint8_t dest, uint8_t base, int32_t offset)
{
    uint32_t addr = (int_gpr[base] * 16) + offset;
    Logger::log(Logger::VU, "LQ: $%08X (%d, %d, $%08X)\n", addr, dest, base, offset);
    for (int i = 0; i < 4; i++)
    {
        if (field & (1 << (3 - i)))
        {
            set_gpr_u(dest, i, read_data<uint32_t>(addr + (i * 4)));
            Logger::log(Logger::OTHER, "(%d)%f ", i, gpr[dest].f[i]);
        }
    }
    Logger::log(Logger::OTHER, "\n");
}

void VectorUnit::lqi(uint8_t field, uint8_t dest, uint8_t base)
{
    Logger::log(Logger::VU, "LQI: ");
    uint32_t addr = int_gpr[base] * 16;
    for (int i = 0; i < 4; i++)
    {
        if (field & (1 << (3 - i)))
        {
            set_gpr_u(dest, i, read_data<uint32_t>(addr + (i * 4)));
            Logger::log(Logger::OTHER, "(%d)%f ", i, gpr[dest].f[i]);
        }
    }
    set_int(base, int_gpr[base] + 1);
    Logger::log(Logger::OTHER, "\n");
}

/**
 * TODO: MADD/MSUB MAC flag calculations
 * The VU User's Manual mentions that flag calculations are performed for both the addition/subtraction and
 * multiplication, which greatly complicates matters.
 */
void VectorUnit::madd(uint8_t field, uint8_t dest, uint8_t reg1, uint8_t reg2)
{
    Logger::log(Logger::VU, "MADD: ");
    for (int i = 0; i < 4; i++)
    {
        if (field & (1 << (3 - i)))
        {
            float temp = convert(gpr[reg1].u[i]) * convert(gpr[reg2].u[i]);
            set_gpr_f(dest, i, temp + convert(ACC.u[i]));
            Logger::log(Logger::OTHER, "(%d)%f ", i, gpr[dest].f[i]);
        }
    }
    Logger::log(Logger::OTHER, "\n");
}

void VectorUnit::madda(uint8_t field, uint8_t reg1, uint8_t reg2)
{
    Logger::log(Logger::VU, "MADDA: ");
    for (int i = 0; i < 4; i++)
    {
        if (field & (1 << (3 - i)))
        {
            float temp = convert(gpr[reg1].u[i]) * convert(gpr[reg2].u[i]);
            ACC.f[i] = temp + convert(ACC.u[i]);
            Logger::log(Logger::OTHER, "(%d)%f ", i, ACC.f[i]);
        }
    }
    Logger::log(Logger::OTHER, "\n");
}

void VectorUnit::maddabc(uint8_t bc, uint8_t field, uint8_t source, uint8_t bc_reg)
{
    Logger::log(Logger::VU, "MADDAbc: ");
    float op = convert(gpr[bc_reg].u[bc]);
    for (int i = 0; i < 4; i++)
    {
        if (field & (1 << (3 - i)))
        {
            float temp = op * convert(gpr[source].u[i]);
            ACC.f[i] = temp + convert(ACC.u[i]);
            Logger::log(Logger::OTHER, "(%d)%f ", i, ACC.f[i]);
        }
    }
    Logger::log(Logger::OTHER, "\n");
}

void VectorUnit::maddai(uint8_t field, uint8_t source)
{
    Logger::log(Logger::VU, "MADDAi: ");
    float op = convert(I.u);
    for (int i = 0; i < 4; i++)
    {
        if (field & (1 << (3 - i)))
        {
            float temp = op * convert(gpr[source].u[i]);
            ACC.f[i] = temp + convert(ACC.u[i]);
            Logger::log(Logger::OTHER, "(%d)%f ", i, ACC.f[i]);
        }
    }
    Logger::log(Logger::OTHER, "\n");
}

void VectorUnit::maddbc(uint8_t bc, uint8_t field, uint8_t dest, uint8_t source, uint8_t bc_reg)
{
    Logger::log(Logger::VU, "MADDbc: ");
    float op = convert(gpr[bc_reg].u[bc]);
    for (int i = 0; i < 4; i++)
    {
        if (field & (1 << (3 - i)))
        {
            float temp = op * convert(gpr[source].u[i]);
            set_gpr_f(dest, i, temp + ACC.f[i]);
            Logger::log(Logger::OTHER, "(%d)%f ", i, gpr[dest].f[i]);
        }
    }
    Logger::log(Logger::OTHER, "\n");
}

void VectorUnit::max(uint8_t field, uint8_t dest, uint8_t reg1, uint8_t reg2)
{
    Logger::log(Logger::VU, "MAX: ");
    for (int i = 0; i < 4; i++)
    {
        if (field & (1 << (3 - i)))
        {
            float op1 = convert(gpr[reg1].u[i]);
            float op2 = convert(gpr[reg2].u[i]);
            if (op1 > op2)
                set_gpr_f(dest, i, op1);
            else
                set_gpr_f(dest, i, op2);
            Logger::log(Logger::OTHER, "(%d)%f ", i, gpr[dest].f[i]);
        }
    }
    Logger::log(Logger::OTHER, "\n");
}

void VectorUnit::maxbc(uint8_t bc, uint8_t field, uint8_t dest, uint8_t source, uint8_t bc_reg)
{
    Logger::log(Logger::VU, "MAXbc: ");
    float op = convert(gpr[bc_reg].u[bc]);
    for (int i = 0; i < 4; i++)
    {
        if (field & (1 << (3 - i)))
        {
            float op2 = convert(gpr[source].u[i]);
            if (op > op2)
                set_gpr_f(dest, i, op);
            else
                set_gpr_f(dest, i, op2);
            Logger::log(Logger::OTHER, "(%d)%f ", i, gpr[dest].f[i]);
        }
    }
    Logger::log(Logger::OTHER, "\n");
}

void VectorUnit::mfir(uint8_t field, uint8_t dest, uint8_t source)
{
    Logger::log(Logger::VU, "MFIR\n");
    for (int i = 0; i < 4; i++)
    {
        if (field & (1 << (3 - i)))
            gpr[dest].s[i] = (int32_t)(int16_t)int_gpr[source];
    }
}

void VectorUnit::mfp(uint8_t field, uint8_t dest)
{
    Logger::log(Logger::VU, "MFP\n");
    for (int i = 0; i < 4; i++)
    {
        if (field & (1 << (3 - i)))
            gpr[dest].f[i] = convert(P.u);
    }
}

void VectorUnit::minibc(uint8_t bc, uint8_t field, uint8_t dest, uint8_t source, uint8_t bc_reg)
{
    Logger::log(Logger::VU, "MINIbc: ");
    float op = convert(gpr[bc_reg].u[bc]);
    for (int i = 0; i < 4; i++)
    {
        if (field & (1 << (3 - i)))
        {
            float op2 = convert(gpr[source].u[i]);
            if (op < op2)
                set_gpr_f(dest, i, op);
            else
                set_gpr_f(dest, i, op2);
            Logger::log(Logger::OTHER, "(%d)%f", i, gpr[dest].f[i]);
        }
    }
    Logger::log(Logger::OTHER, "\n");
}

void VectorUnit::mini(uint8_t field, uint8_t dest, uint8_t reg1, uint8_t reg2)
{
    Logger::log(Logger::VU, "MINI: ");
    for (int i = 0; i < 4; i++)
    {
        if (field & (1 << (3 - i)))
        {
            float op1 = convert(gpr[reg1].u[i]);
            float op2 = convert(gpr[reg2].u[i]);
            if (op1 < op2)
                set_gpr_f(dest, i, op1);
            else
                set_gpr_f(dest, i, op2);
            Logger::log(Logger::OTHER, "(%d)%f ", i, gpr[dest].f[i]);
        }
    }
    Logger::log(Logger::OTHER, "\n");
}

void VectorUnit::minii(uint8_t field, uint8_t dest, uint8_t source)
{
    Logger::log(Logger::VU, "MINIi: ");
    float op = convert(I.u);
    for (int i = 0; i < 4; i++)
    {
        if (field & (1 << (3 - i)))
        {
            float op2 = convert(gpr[source].u[i]);
            if (op < op2)
                set_gpr_f(dest, i, op);
            else
                set_gpr_f(dest, i, op2);
            Logger::log(Logger::OTHER, "(%d)%f", i, gpr[dest].f[i]);
        }
    }
    Logger::log(Logger::OTHER, "\n");
}

void VectorUnit::move(uint8_t field, uint8_t dest, uint8_t source)
{
    Logger::log(Logger::VU, "MOVE");
    for (int i = 0; i < 4; i++)
    {
        if (field & (1 << (3 - i)))
            set_gpr_u(dest, i, gpr[source].u[i]);
    }
    Logger::log(Logger::OTHER, "\n");
}

void VectorUnit::mr32(uint8_t field, uint8_t dest, uint8_t source)
{
    Logger::log(Logger::VU, "MR32");
    uint32_t x = gpr[source].u[0];
    if (_x(field))
        set_gpr_f(dest, 0, convert(gpr[source].u[1]));
    if (_y(field))
        set_gpr_f(dest, 1, convert(gpr[source].u[2]));
    if (_z(field))
        set_gpr_f(dest, 2, convert(gpr[source].u[3]));
    if (_w(field))
        set_gpr_f(dest, 3, convert(x));
    Logger::log(Logger::OTHER, "\n");
}

void VectorUnit::msubabc(uint8_t bc, uint8_t field, uint8_t source, uint8_t bc_reg)
{
    Logger::log(Logger::VU, "MSUBAbc: ");
    float op = convert(gpr[bc_reg].u[bc]);
    for (int i = 0; i < 4; i++)
    {
        if (field & (1 << (3 - i)))
        {
            float temp = op * convert(gpr[source].u[i]);
            ACC.f[i] = convert(ACC.u[i]) - temp;
            Logger::log(Logger::OTHER, "(%d)%f ", i, ACC.f[i]);
        }
    }
    Logger::log(Logger::OTHER, "\n");
}

void VectorUnit::msubai(uint8_t field, uint8_t source)
{
    Logger::log(Logger::VU, "MSUBAi: ");
    float op = convert(I.u);
    for (int i = 0; i < 4; i++)
    {
        if (field & (1 << (3 - i)))
        {
            float temp = op * convert(gpr[source].u[i]);
            ACC.f[i] = convert(ACC.u[i]) - temp;
            Logger::log(Logger::OTHER, "(%d)%f ", i, ACC.f[i]);
        }
    }
    Logger::log(Logger::OTHER, "\n");
}

void VectorUnit::msubbc(uint8_t bc, uint8_t field, uint8_t dest, uint8_t source, uint8_t bc_reg)
{
    Logger::log(Logger::VU, "MSUBbc: ");
    float op = convert(gpr[bc_reg].u[bc]);
    for (int i = 0; i < 4; i++)
    {
        if (field & (1 << (3 - i)))
        {
            float temp = op * convert(gpr[source].u[i]);
            set_gpr_f(dest, i, ACC.f[i] - temp);
            Logger::log(Logger::OTHER, "(%d)%f ", i, gpr[dest].f[i]);
        }
    }
    Logger::log(Logger::OTHER, "\n");
}

void VectorUnit::msubi(uint8_t field, uint8_t dest, uint8_t source)
{
    Logger::log(Logger::VU, "MSUBi: ");
    float op = convert(I.u);
    for (int i = 0; i < 4; i++)
    {
        if (field & (1 << (3 - i)))
        {
            float temp = op * convert(gpr[source].u[i]);
            set_gpr_f(dest, i, ACC.f[i] - temp);
            Logger::log(Logger::OTHER, "(%d)%f ", i, gpr[dest].f[i]);
        }
    }
    Logger::log(Logger::OTHER, "\n");
}

void VectorUnit::mtir(uint8_t fsf, uint8_t dest, uint8_t source)
{
    Logger::log(Logger::VU, "MTIR: %d\n", gpr[source].u[fsf] & 0xFFFF);
    set_int(dest, gpr[source].u[fsf] & 0xFFFF);
}

void VectorUnit::mul(uint8_t field, uint8_t dest, uint8_t reg1, uint8_t reg2)
{
    Logger::log(Logger::VU, "MUL: ");
    for (int i = 0; i < 4; i++)
    {
        if (field & (1 << (3 - i)))
        {
            float result = convert(gpr[reg1].u[i]) * convert(gpr[reg2].u[i]);
            update_mac_flags(result, i);
            set_gpr_f(dest, i, result);
            Logger::log(Logger::OTHER, "(%d)%f ", i, gpr[dest].f[i]);
        }
        else
            clear_mac_flags(i);
    }
    Logger::log(Logger::OTHER, "\n");
}

void VectorUnit::mula(uint8_t field, uint8_t reg1, uint8_t reg2)
{
    Logger::log(Logger::VU, "MULA: ");
    for (int i = 0; i < 4; i++)
    {
        if (field & (1 << (3 - i)))
        {
            float temp = convert(gpr[reg1].u[i]) * convert(gpr[reg2].u[i]);
            update_mac_flags(temp, i);
            ACC.f[i] = temp;
            Logger::log(Logger::OTHER, "(%d)%f ", i, ACC.f[i]);
        }
        else
            clear_mac_flags(i);
    }
    Logger::log(Logger::OTHER, "\n");
}

void VectorUnit::mulabc(uint8_t bc, uint8_t field, uint8_t source, uint8_t bc_reg)
{
    Logger::log(Logger::VU, "MULAbc: ");
    float op = convert(gpr[bc_reg].u[bc]);
    for (int i = 0; i < 4; i++)
    {
        if (field & (1 << (3 - i)))
        {
            float temp = op * convert(gpr[source].u[i]);
            update_mac_flags(temp, i);
            ACC.f[i] = temp;
            Logger::log(Logger::OTHER, "(%d)%f ", i, ACC.f[i]);
        }
        else
            clear_mac_flags(i);
    }
    Logger::log(Logger::OTHER, "\n");
}

void VectorUnit::mulai(uint8_t field, uint8_t source)
{
    Logger::log(Logger::VU, "MULAi: ");
    float op = convert(I.u);
    for (int i = 0; i < 4; i++)
    {
        if (field & (1 << (3 - i)))
        {
            float temp = convert(gpr[source].u[i]) * op;
            update_mac_flags(temp, i);
            ACC.f[i] = temp;
            Logger::log(Logger::OTHER, "(%d)%f ", i, ACC.f[i]);
        }
        else
            clear_mac_flags(i);
    }
    Logger::log(Logger::OTHER, "\n");
}

void VectorUnit::mulbc(uint8_t bc, uint8_t field, uint8_t dest, uint8_t source, uint8_t bc_reg)
{
    Logger::log(Logger::VU, "MULbc: ");
    float op = convert(gpr[bc_reg].u[bc]);
    for (int i = 0; i < 4; i++)
    {
        if (field & (1 << (3 - i)))
        {
            float temp = op * convert(gpr[source].u[i]);
            update_mac_flags(temp, i);
            set_gpr_f(dest, i, temp);
            Logger::log(Logger::OTHER, "(%d)%f ", i, gpr[dest].f[i]);
        }
        else
            clear_mac_flags(i);
    }
    Logger::log(Logger::OTHER, "\n");
}

void VectorUnit::muli(uint8_t field, uint8_t dest, uint8_t source)
{
    Logger::log(Logger::VU, "MULi: ");
    float op = convert(I.u);
    for (int i = 0; i < 4; i++)
    {
        if (field & (1 << (3 - i)))
        {
            float temp = op * convert(gpr[source].u[i]);
            update_mac_flags(temp, i);
            set_gpr_f(dest, i, temp);
            Logger::log(Logger::OTHER, "(%d)%f ", i, gpr[dest].f[i]);
        }
        else
            clear_mac_flags(i);
    }
    Logger::log(Logger::OTHER, "\n");
}

void VectorUnit::mulq(uint8_t field, uint8_t dest, uint8_t source)
{
    Logger::log(Logger::VU, "MULq: ");
    float op = convert(Q.u);
    for (int i = 0; i < 4; i++)
    {
        if (field & (1 << (3 - i)))
        {
            float temp = op * convert(gpr[source].u[i]);
            update_mac_flags(temp, i);
            set_gpr_f(dest, i, temp);
            Logger::log(Logger::OTHER, "(%d)%f ", i, gpr[dest].f[i]);
        }
        else
            clear_mac_flags(i);
    }
    Logger::log(Logger::OTHER, "\n");
}

/**
 * FDx = ACCx - FSy * FTz
 * FDy = ACCy - FSz * FTx
 * FDz = ACCz - FSx * FTy
 */
void VectorUnit::opmsub(uint8_t dest, uint8_t reg1, uint8_t reg2)
{
    set_gpr_f(dest, 0, convert(ACC.u[0]) - convert(gpr[reg1].u[1]) * convert(gpr[reg2].u[2]));
    set_gpr_f(dest, 1, convert(ACC.u[1]) - convert(gpr[reg1].u[2]) * convert(gpr[reg2].u[0]));
    set_gpr_f(dest, 2, convert(ACC.u[2]) - convert(gpr[reg1].u[0]) * convert(gpr[reg2].u[1]));

    update_mac_flags(gpr[dest].f[0], 0);
    update_mac_flags(gpr[dest].f[1], 1);
    update_mac_flags(gpr[dest].f[2], 2);
    Logger::log(Logger::VU, "OPMSUB: %f, %f, %f\n", gpr[dest].f[0], gpr[dest].f[1], gpr[dest].f[2]);
}

/**
 * ACCx = FSy * FTz
 * ACCy = FSz * FTx
 * ACCz = FSx * FTy
 */
void VectorUnit::opmula(uint8_t reg1, uint8_t reg2)
{
    ACC.f[0] = convert(gpr[reg1].u[1]) * convert(gpr[reg2].u[2]);
    ACC.f[1] = convert(gpr[reg1].u[2]) * convert(gpr[reg2].u[0]);
    ACC.f[2] = convert(gpr[reg1].u[0]) * convert(gpr[reg2].u[1]);

    update_mac_flags(ACC.f[0], 0);
    update_mac_flags(ACC.f[1], 1);
    update_mac_flags(ACC.f[2], 2);
    Logger::log(Logger::VU, "OPMULA: %f, %f, %f\n", ACC.f[0], ACC.f[1], ACC.f[2]);
}

void VectorUnit::rget(uint8_t field, uint8_t dest)
{
    for (int i = 0; i < 4; i++)
    {
        if (field & (1 << (3 - i)))
        {
            set_gpr_u(dest, i, R.u);
        }
    }
    Logger::log(Logger::VU, "RGET: %f\n", R.f);
}

void VectorUnit::rinit(uint8_t fsf, uint8_t source)
{
    R.u = 0x3F800000;
    R.u |= gpr[source].u[fsf] & 0x007FFFFF;
    Logger::log(Logger::VU, "RINIT: %f\n", R.f);
}

void VectorUnit::rnext(uint8_t field, uint8_t dest)
{
    advance_r();
    for (int i = 0; i < 4; i++)
    {
        if (field & (1 << (3 - i)))
        {
            set_gpr_u(dest, i, R.u);
        }
    }
    Logger::log(Logger::VU, "RNEXT: %f\n", R.f);
}

void VectorUnit::rsqrt(uint8_t ftf, uint8_t fsf, uint8_t reg1, uint8_t reg2)
{
    float denom = fabs(convert(gpr[reg2].u[ftf]));
    float num = convert(gpr[reg1].u[fsf]);

    status = (status & 0xFCF) | ((status & 0x30) << 6);

    if (!denom)
    {
        Logger::log(Logger::VU, "RSQRT by zero!\n");

        if (num == 0.0)
            status |= 0x10;
        else
            status |= 0x20;
        
        if ((gpr[reg1].u[fsf] & 0x80000000) != (gpr[reg2].u[ftf] & 0x80000000))
            Q.u = 0xFF7FFFFF;
        else
            Q.u = 0x7F7FFFFF;
    }
    else
    {
        Q.f = num;
        Q.f /= sqrt(denom);
    }
    Logger::log(Logger::VU, "RSQRT: %f\n", Q.f);
    Logger::log(Logger::OTHER, "Reg1: %f\n", gpr[reg1].f[fsf]);
    Logger::log(Logger::OTHER, "Reg2: %f\n", gpr[reg2].f[ftf]);
}

void VectorUnit::rxor(uint8_t fsf, uint8_t source)
{
    VU_R temp;
    temp.u = (R.u & 0x007FFFFF) | 0x3F800000;
    R.u = temp.u ^ (gpr[source].u[fsf] & 0x007FFFFF);
    Logger::log(Logger::VU, "RXOR: %f\n", R.f);
}

void VectorUnit::sq(uint8_t field, uint8_t source, uint8_t base, int32_t offset)
{
    uint32_t addr = (int_gpr[base] << 4) + offset;
    Logger::log(Logger::VU, "SQ to $%08X!\n", addr);
    for (int i = 0; i < 4; i++)
    {
        if (field & (1 << (3 - i)))
        {
            write_data<uint32_t>(addr + (i * 4), gpr[source].u[i]);
        }
    }
}

void VectorUnit::sqi(uint8_t field, uint8_t source, uint8_t base)
{
    uint32_t addr = int_gpr[base] << 4;
    Logger::log(Logger::VU, "SQI to $%08X!\n", addr);
    for (int i = 0; i < 4; i++)
    {
        if (field & (1 << (3 - i)))
        {
            write_data<uint32_t>(addr + (i * 4), gpr[source].u[i]);
        }
    }
    if (base)
        int_gpr[base]++;
}

void VectorUnit::vu_sqrt(uint8_t ftf, uint8_t source)
{
    Q.f = sqrt(fabs(convert(gpr[source].u[ftf])));
    Logger::log(Logger::VU, "SQRT: %f\n", Q.f);
    Logger::log(Logger::OTHER, "Source: %f\n", gpr[source].f[ftf]);
}

void VectorUnit::sub(uint8_t field, uint8_t dest, uint8_t reg1, uint8_t reg2)
{
    Logger::log(Logger::VU, "SUB: ");
    for (int i = 0; i < 4; i++)
    {
        if (field & (1 << (3 - i)))
        {
            float result = convert(gpr[reg1].u[i]) - convert(gpr[reg2].u[i]);
            update_mac_flags(result, i);
            set_gpr_f(dest, i, result);
            Logger::log(Logger::OTHER, "(%d)%f ", i, gpr[dest].f[i]);
        }
        else
            clear_mac_flags(i);
    }
    Logger::log(Logger::OTHER, "\n");
}

void VectorUnit::subbc(uint8_t bc, uint8_t field, uint8_t dest, uint8_t source, uint8_t bc_reg)
{
    Logger::log(Logger::VU, "SUBbc: ");
    float op = convert(gpr[bc_reg].u[bc]);
    for (int i = 0; i < 4; i++)
    {
        if (field & (1 << (3 - i)))
        {
            float temp = convert(gpr[source].u[i]) - op;
            update_mac_flags(temp, i);
            set_gpr_f(dest, i, temp);
            Logger::log(Logger::OTHER, "(%d)%f ", i, gpr[dest].f[i]);
        }
        else
            clear_mac_flags(i);
    }
    Logger::log(Logger::OTHER, "\n");
}

void VectorUnit::subi(uint8_t field, uint8_t dest, uint8_t source)
{
    Logger::log(Logger::VU, "SUBi: ");
    float op = convert(I.u);
    for (int i = 0; i < 4; i++)
    {
        if (field & (1 << (3 - i)))
        {
            float temp = convert(gpr[source].u[i]) - op;
            update_mac_flags(temp, i);
            set_gpr_f(dest, i, temp);
            Logger::log(Logger::OTHER, "(%d)%f ", i, gpr[dest].f[i]);
        }
        else
            clear_mac_flags(i);
    }
    Logger::log(Logger::OTHER, "\n");
}

void VectorUnit::subq(uint8_t field, uint8_t dest, uint8_t source)
{
    Logger::log(Logger::VU, "SUBq: ");
    float value = convert(Q.u);
    for (int i = 0; i < 4; i++)
    {
        if (field & (1 << (3 - i)))
        {
            float temp = convert(gpr[source].u[i]) - value;
            update_mac_flags(temp, i);
            set_gpr_f(dest, i, temp);
            Logger::log(Logger::OTHER, "(%d)%f ", i, gpr[dest].f[i]);
        }
        else
            clear_mac_flags(i);
    }
    Logger::log(Logger::OTHER, "\n");
}

void VectorUnit::xgkick(uint8_t is)
{
    if (!id)
    {
        Logger::log(Logger::VU, "ERROR: XGKICK called on VU0!\n");
        exit(1);
    }
    Logger::log(Logger::VU1, "XGKICK: Addr $%08X\n", int_gpr[is] * 16);
    gif->activate_PATH(1);
    transferring_GIF = true;
    GIF_addr = int_gpr[is] * 16;
}

void VectorUnit::xitop(uint8_t it)
{
    Logger::log(Logger::VU, "XTIOP: $%04X (%d)\n", *VIF_ITOP, it);
    set_int(it, *VIF_ITOP);
}

void VectorUnit::xtop(uint8_t it)
{
    if (!id)
    {
        Logger::log(Logger::VU, "ERROR: XTOP called on VU0!\n");
        exit(1);
    }
    Logger::log(Logger::VU1, "XTOP: $%04X (%d)\n", *VIF_TOP, it);
    set_int(it, *VIF_TOP);
}
