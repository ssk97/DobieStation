#include "../logger.hpp"
#include <cstdlib>
#include "dmac.hpp"

#include "../emulator.hpp"

enum CHANNELS
{
    VIF0,
    VIF1,
    GIF,
    IPU_FROM,
    IPU_TO,
    SIF0,
    SIF1,
    SIF2,
    SPR_FROM,
    SPR_TO,
    MFIFO_EMPTY = 14
};

DMAC::DMAC(EmotionEngine* cpu, Emulator* e, GraphicsInterface* gif, ImageProcessingUnit* ipu, SubsystemInterface* sif,
           VectorInterface* vif0, VectorInterface* vif1) :
    RDRAM(nullptr), scratchpad(nullptr), cpu(cpu), e(e), gif(gif), ipu(ipu), sif(sif), vif0(vif0), vif1(vif1)
{

}

void DMAC::reset(uint8_t* RDRAM, uint8_t* scratchpad)
{
    this->RDRAM = RDRAM;
    this->scratchpad = scratchpad;
    master_disable = 0x1201; //hax
    control.master_enable = false;
    mfifo_empty_triggered = false;
    PCR = 0;

    for (int i = 0; i < 15; i++)
    {
        channels[i].control = 0;
        interrupt_stat.channel_mask[i] = false;
        interrupt_stat.channel_stat[i] = false;
    }
}

uint128_t DMAC::fetch128(uint32_t addr)
{
    if (addr & (1 << 31))
    {
        addr &= 0x3FF0;
        return *(uint128_t*)&scratchpad[addr];
    }
    else
    {
        addr &= 0x01FFFFF0;
        return *(uint128_t*)&RDRAM[addr];
    }
}

void DMAC::store128(uint32_t addr, uint128_t data)
{
    if (addr & (1 << 31))
    {
        addr &= 0x3FF0;
        *(uint128_t*)&scratchpad[addr] = data;
    }
    else
    {
        addr &= 0x01FFFFF0;
        *(uint128_t*)&RDRAM[addr] = data;
    }
}

void DMAC::run()
{
    /*if (!control.master_enable || (master_disable & (1 << 16)))
        return;
    for (int i = 0; i < 10; i++)
    {
        if (channels[i].control & 0x100)
        {
            switch (i)
            {
                case VIF1:
                    process_VIF1();
                    break;
                case GIF:
                    process_GIF();
                    break;
                case SIF0:
                    process_SIF0();
                    break;
                case SIF1:
                    process_SIF1();
                    break;
            }
        }
    }*/
}

void DMAC::run(int cycles)
{
    if (!control.master_enable || (master_disable & (1 << 16)))
        return;
    int c;
    for (int i = 0; i < 10; i++)
    {
        c = cycles;
        while ((channels[i].control & 0x100) && c)
        {
            switch (i)
            {
                case VIF0:
                    process_VIF0();
                    break;
                case VIF1:
                    process_VIF1();
                    break;
                case GIF:
                    process_GIF();
                    break;
                case IPU_FROM:
                    process_IPU_FROM();
                    break;
                case IPU_TO:
                    process_IPU_TO();
                    break;
                case SIF0:
                    process_SIF0();
                    break;
                case SIF1:
                    process_SIF1();
                    break;
                case SPR_FROM:
                    process_SPR_FROM();
                    break;
                case SPR_TO:
                    process_SPR_TO();
                    break;
            }
            c--;
        }
    }
}

//mfifo_handler will return false if the MFIFO is empty and the MFIFO is in use. DMACwise it returns true
bool DMAC::mfifo_handler(int index)
{
    if (control.mem_drain_channel - 1 == index)
    {
        uint8_t id = (channels[index].control >> 28) & 0x7;

        channels[index].tag_address = RBOR | (channels[index].tag_address & RBSR);

        uint32_t addr = channels[index].tag_address;

        //Don't mask the MADR on REFE/REF/REFS as they don't follow the tag, so likely outside the MFIFO
        if (channels[index].quadword_count)
        {
            if (id != 0 && id != 3 && id != 4)
                channels[index].address = RBOR | (channels[index].address & RBSR);

            addr = channels[index].address;
        }

        if (addr == channels[SPR_FROM].address)
        {
            if (!mfifo_empty_triggered)
            {
                interrupt_stat.channel_stat[MFIFO_EMPTY] = true;
                int1_check();
                mfifo_empty_triggered = true;
                Logger::log(Logger::DMAC, "MFIFO Empty\n");
            }
            return false;
        }
        mfifo_empty_triggered = false;
    }

    return true;
}

void DMAC::transfer_end(int index)
{
    Logger::log(Logger::DMAC, "Transfer end: %d\n", index);
    channels[index].control &= ~0x100;
    interrupt_stat.channel_stat[index] = true;
    int1_check();
}

void DMAC::int1_check()
{
    bool int1_signal = false;
    for (int i = 0; i < 15; i++)
    {
        if (interrupt_stat.channel_stat[i] & interrupt_stat.channel_mask[i])
        {
            int1_signal = true;
            break;
        }
    }
    cpu->set_int1_signal(int1_signal);
}

void DMAC::process_VIF0()
{
    if (channels[VIF0].quadword_count)
    {
        vif0->feed_DMA(fetch128(channels[VIF0].address));

        channels[VIF0].address += 16;
        channels[VIF0].quadword_count--;
    }
    else
    {
        if (channels[VIF0].tag_end)
            transfer_end(VIF0);
        else
        {
            uint128_t DMAtag = fetch128(channels[VIF0].tag_address);
            if (channels[VIF0].control & (1 << 6))
                vif0->transfer_DMAtag(DMAtag);
            handle_source_chain(VIF0);
        }
    }
}

void DMAC::process_VIF1()
{
    if (!mfifo_handler(VIF1))
        return;

    if (channels[VIF1].quadword_count)
    {
        vif1->feed_DMA(fetch128(channels[VIF1].address));

        channels[VIF1].address += 16;
        channels[VIF1].quadword_count--;
    }
    else
    {
        if (channels[VIF1].tag_end)
            transfer_end(VIF1);
        else
        {
            uint128_t DMAtag = fetch128(channels[VIF1].tag_address);
            if (channels[VIF1].control & (1 << 6))
                vif1->transfer_DMAtag(DMAtag);
            handle_source_chain(VIF1);
        }
    }
}

void DMAC::process_GIF()
{
    if (!mfifo_handler(GIF))
        return;

    if (channels[GIF].quadword_count)
    {
        gif->send_PATH3(fetch128(channels[GIF].address));

        channels[GIF].address += 16;
        channels[GIF].quadword_count--;
    }
    else
    {
        if (channels[GIF].tag_end)
        {
            transfer_end(GIF);
            gif->deactivate_PATH(3);
        }
        else
        {
            handle_source_chain(GIF);
        }
    }
}

void DMAC::process_IPU_FROM()
{
    if (channels[IPU_FROM].quadword_count)
    {
        if (ipu->can_read_FIFO())
        {
            uint128_t data = ipu->read_FIFO();
            store128(channels[IPU_FROM].address, data);
            //Logger::log(Logger::IPU_FROM, "Store $%08X_$%08X to $%08X\n", data._u32[1], data._u32[0], channels[IPU_FROM].address);

            channels[IPU_FROM].address += 16;
            channels[IPU_FROM].quadword_count--;
        }
    }
    else
    {
        if (channels[IPU_FROM].tag_end)
            transfer_end(IPU_FROM);
        else
        {
            Logger::log(Logger::DMAC, "blorp\n");
            exit(1);
        }
    }
}

void DMAC::process_IPU_TO()
{
    if (channels[IPU_TO].quadword_count)
    {
        if (ipu->can_write_FIFO())
        {
            ipu->write_FIFO(fetch128(channels[IPU_TO].address));

            channels[IPU_TO].address += 16;
            channels[IPU_TO].quadword_count--;
        }
    }
    else
    {
        if (channels[IPU_TO].tag_end)
            transfer_end(IPU_TO);
        else
            handle_source_chain(IPU_TO);
    }
}

void DMAC::process_SIF0()
{
    if (channels[SIF0].quadword_count)
    {
        if (sif->get_SIF0_size() >= 4)
        {
            for (int i = 0; i < 4; i++)
            {
                uint32_t word = sif->read_SIF0();
                e->write32(channels[SIF0].address, word);
                channels[SIF0].address += 4;
            }
            channels[SIF0].quadword_count--;
        }
    }
    else
    {
        if (channels[SIF0].tag_end)
        {
            transfer_end(SIF0);
        }
        else if (sif->get_SIF0_size() >= 2)
        {
            uint64_t DMAtag = sif->read_SIF0();
            DMAtag |= (uint64_t)sif->read_SIF0() << 32;
            Logger::log(Logger::DMAC, "SIF0 tag: $%08X_%08X\n", DMAtag >> 32, DMAtag);

            channels[SIF0].quadword_count = DMAtag & 0xFFFF;
            channels[SIF0].address = DMAtag >> 32;
            channels[SIF0].tag_address += 16;

            int mode = (DMAtag >> 28) & 0x7;

            bool IRQ = (DMAtag & (1UL << 31));
            bool TIE = channels[SIF0].control & (1 << 7);
            if (mode == 7 || (IRQ && TIE))
                channels[SIF0].tag_end = true;

            channels[SIF0].control &= 0xFFFF;
            channels[SIF0].control |= DMAtag & 0xFFFF0000;
        }
    }
}

void DMAC::process_SIF1()
{
    if (channels[SIF1].quadword_count)
    {
        if (sif->get_SIF1_size() <= SubsystemInterface::MAX_FIFO_SIZE - 4)
        {
            sif->write_SIF1(fetch128(channels[SIF1].address));

            channels[SIF1].address += 16;
            channels[SIF1].quadword_count--;
        }
    }
    else
    {
        if (channels[SIF1].tag_end)
        {
            transfer_end(SIF1);
        }
        else
            handle_source_chain(SIF1);
    }
}

void DMAC::process_SPR_FROM()
{
    if (channels[SPR_FROM].quadword_count)
    {
        if (control.mem_drain_channel != 0)
        {
            channels[SPR_FROM].address = RBOR | (channels[SPR_FROM].address & RBSR);
        }

        uint128_t DMAData = fetch128(channels[SPR_FROM].scratchpad_address | (1 << 31));
        store128(channels[SPR_FROM].address, DMAData);

        channels[SPR_FROM].scratchpad_address += 16;
        channels[SPR_FROM].quadword_count--;

        if (control.mem_drain_channel != 0)
            channels[SPR_FROM].address = RBOR | ((channels[SPR_FROM].address + 16) & RBSR);
        else
            channels[SPR_FROM].address += 16;
    }
    else
    {
        if (channels[SPR_FROM].tag_end)
        {
            transfer_end(SPR_FROM);
        }
        else
        {
            uint128_t DMAtag = fetch128(channels[SPR_FROM].scratchpad_address);
            Logger::log(Logger::DMAC, "SPR_FROM tag: $%08X_%08X\n", DMAtag._u32[1], DMAtag._u32[0]);

            channels[SPR_FROM].quadword_count = DMAtag._u32[0] & 0xFFFF;
            channels[SPR_FROM].address = DMAtag._u32[1];
            channels[SPR_FROM].scratchpad_address += 16;
            channels[SPR_FROM].scratchpad_address &= 0x3FFF;

            int mode = (DMAtag._u32[0] >> 28) & 0x7;

            bool IRQ = (DMAtag._u32[0] & (1UL << 31));
            bool TIE = channels[SPR_FROM].control & (1 << 7);
            if (mode == 7 || (IRQ && TIE))
                channels[SPR_FROM].tag_end = true;

            channels[SPR_FROM].control &= 0xFFFF;
            channels[SPR_FROM].control |= DMAtag._u32[0] & 0xFFFF0000;
        }
    }
}

void DMAC::process_SPR_TO()
{
    if (channels[SPR_TO].quadword_count)
    {
        uint128_t DMAData = fetch128(channels[SPR_TO].address);
        store128(channels[SPR_TO].scratchpad_address | (1 << 31), DMAData);
        channels[SPR_TO].scratchpad_address += 16;
        channels[SPR_TO].address += 16;
        channels[SPR_TO].quadword_count--;
    }
    else
    {
        if (channels[SPR_TO].tag_end)
            transfer_end(SPR_TO);
        else
        {
            uint128_t DMAtag = fetch128(channels[SPR_TO].tag_address);
            if (channels[SPR_TO].control & (1 << 6))
            {
                store128(channels[SPR_TO].scratchpad_address | (1 << 31), DMAtag);
                channels[SPR_TO].scratchpad_address += 16;
            }
            handle_source_chain(SPR_TO);
        }
    }
}

void DMAC::handle_source_chain(int index)
{
    uint128_t quad = fetch128(channels[index].tag_address);
    uint64_t DMAtag = quad._u64[0];
    Logger::log(Logger::DMAC, "Source DMAtag read $%08X: $%08X_%08X\n", channels[index].tag_address, DMAtag >> 32, DMAtag & 0xFFFFFFFF);

    //Change CTRL to have the upper 16 bits equal to bits 16-31 of the most recently read DMAtag
    channels[index].control &= 0xFFFF;
    channels[index].control |= DMAtag & 0xFFFF0000;

    uint16_t quadword_count = DMAtag & 0xFFFF;
    uint8_t id = (DMAtag >> 28) & 0x7;
    uint32_t addr = (DMAtag >> 32) & 0xFFFFFFF0;
    bool IRQ_after_transfer = DMAtag & (1UL << 31);
    bool TIE = channels[index].control & (1 << 7);
    channels[index].quadword_count = quadword_count;
    switch (id)
    {
        case 0:
            //refe
            channels[index].address = addr;
            channels[index].tag_address += 16;
            channels[index].tag_end = true;
            break;
        case 1:
            //cnt
            channels[index].address = channels[index].tag_address + 16;
            channels[index].tag_address = channels[index].address + (channels[index].quadword_count << 4);
            break;
        case 2:
            //next
            channels[index].address = channels[index].tag_address + 16;
            channels[index].tag_address = addr;
            break;
        case 3:
            //ref
            channels[index].address = addr;
            channels[index].tag_address += 16;
            break;
        case 4:
            //refs
            channels[index].address = addr;
            channels[index].tag_address += 16;
            break;
        case 5:
        {
            //call
            channels[index].address = channels[index].tag_address + 16;

            int asp = (channels[index].control >> 4) & 0x3;
            uint32_t saved_addr = channels[index].address + (channels[index].quadword_count << 4);
            switch (asp)
            {
                case 0:
                    channels[index].tag_save0 = saved_addr;
                    break;
                case 1:
                    channels[index].tag_save1 = saved_addr;
                    break;
                case 2:
                    Logger::log(Logger::DMAC, "DMAtag 'call' sent when ASP == 2!\n");
                    exit(1);
            }
            asp++;
            channels[index].control &= ~(0x3 << 4);
            channels[index].control |= asp << 4;

            channels[index].tag_address = addr;
        }
            break;
        case 6:
        {
            //ret
            channels[index].address = channels[index].tag_address + 16;
            int asp = (channels[index].control >> 4) & 0x3;
            switch (asp)
            {
                case 0:
                    channels[index].tag_end = true;
                    break;
                case 1:
                    channels[index].tag_address = channels[index].tag_save0;
                    asp--;
                    break;
                case 2:
                    channels[index].tag_address = channels[index].tag_save1;
                    asp--;
                    break;
            }
            channels[index].control &= ~(0x3 << 4);
            channels[index].control |= asp << 4;
        }
            break;
        case 7:
            //end
            channels[index].address = channels[index].tag_address + 16;
            channels[index].tag_end = true;
            break;
        default:
            Logger::log(Logger::DMAC, "\n[DMAC] Unrecognized source chain DMAtag id %d\n", id);
            exit(1);
    }
    if (IRQ_after_transfer && TIE)
        channels[index].tag_end = true;
    Logger::log(Logger::DMAC, "New address: $%08X\n", channels[index].address);
    Logger::log(Logger::DMAC, "New tag addr: $%08X\n", channels[index].tag_address);
}

void DMAC::start_DMA(int index)
{
    Logger::log(Logger::DMAC, "D%d started: $%08X\n", index, channels[index].control);
    Logger::log(Logger::DMAC, "Addr: $%08X\n", channels[index].address);
    Logger::log(Logger::DMAC, "Mode: %d\n", (channels[index].control >> 2) & 0x3);
    Logger::log(Logger::DMAC, "ASP: %d\n", (channels[index].control >> 4) & 0x3);
    Logger::log(Logger::DMAC, "TTE: %d\n", channels[index].control & (1 << 6));
    int mode = (channels[index].control >> 2) & 0x3;
    channels[index].tag_end = (mode == 0); //always end transfers in normal mode
    if (mode == 2) 
    {
        Logger::log(Logger::DMAC, "D%d Unhandled Interleave Mode", index);
    }
}

uint32_t DMAC::read_master_disable()
{
    return master_disable;
}

void DMAC::write_master_disable(uint32_t value)
{
    master_disable = value;
}

uint8_t DMAC::read8(uint32_t address)
{
    uint8_t reg = 0;
    switch (address)
    {
        case 0x10009000:
            reg = channels[VIF1].control & 0xFF;
            break;
        default:
            Logger::log(Logger::DMAC, "Unrecognized read8 from $%08X\n", address);
    }
    return reg;
}

uint32_t DMAC::read32(uint32_t address)
{
    uint32_t reg = 0;
    switch (address)
    {
        case 0x10009000:
            reg = channels[VIF1].control;
            break;
        case 0x10009010:
            reg = channels[VIF1].address;
            break;
        case 0x10009020:
            reg = channels[VIF1].quadword_count;
            break;
        case 0x10009030:
            reg = channels[VIF1].tag_address;
            break;
        case 0x1000A000:
            //Logger::log(Logger::DMAC, "Read GIF control\n");
            reg = channels[GIF].control;
            break;
        case 0x1000A010:
            reg = channels[GIF].address;
            break;
        case 0x1000A020:
            reg = channels[GIF].quadword_count;
            break;
        case 0x1000A030:
            reg = channels[GIF].tag_address;
            break;
        case 0x1000B000:
            reg = channels[IPU_FROM].control;
            break;
        case 0x1000B010:
            reg = channels[IPU_FROM].address;
            break;
        case 0x1000B020:
            reg = channels[IPU_FROM].quadword_count;
            break;
        case 0x1000B030:
            reg = channels[IPU_FROM].tag_address;
            break;
        case 0x1000B400:
            reg = channels[IPU_TO].control;
            break;
        case 0x1000B410:
            reg = channels[IPU_TO].address;
            break;
        case 0x1000B420:
            reg = channels[IPU_TO].quadword_count;
            break;
        case 0x1000B430:
            reg = channels[IPU_TO].tag_address;
            break;
        case 0x1000C000:
            reg = channels[SIF0].control;
            break;
        case 0x1000C020:
            reg = channels[SIF0].quadword_count;
            break;
        case 0x1000C400:
            reg = channels[SIF1].control;
            break;
        case 0x1000C410:
            reg = channels[SIF1].address;
            break;
        case 0x1000C420:
            reg = channels[SIF1].quadword_count;
            break;
        case 0x1000C430:
            reg = channels[SIF1].tag_address;
            break;
        case 0x1000D000:
            reg = channels[SPR_FROM].control;
            break;
        case 0x1000D010:
            reg = channels[SPR_FROM].address;
            break;
        case 0x1000D020:
            reg = channels[SPR_FROM].quadword_count;
            break;
        case 0x1000D400:
            reg = channels[SPR_TO].control;
            break;
        case 0x1000D410:
            reg = channels[SPR_TO].address;
            break;
        case 0x1000D420:
            reg = channels[SPR_TO].quadword_count;
            break;
        case 0x1000D430:
            reg = channels[SPR_TO].tag_address;
            break;
        case 0x1000E000:
            reg |= control.master_enable;
            reg |= control.cycle_stealing << 1;
            reg |= control.mem_drain_channel << 2;
            reg |= control.stall_source_channel << 4;
            reg |= control.stall_dest_channel << 6;
            reg |= control.release_cycle << 8;
            break;
        case 0x1000E010:
            for (int i = 0; i < 15; i++)
            {
                reg |= interrupt_stat.channel_stat[i] << i;
                reg |= interrupt_stat.channel_mask[i] << (i + 16);
            }
            break;
        case 0x1000E020:
            reg = PCR;
            break;
        case 0x1000E040:
            reg = RBSR;
            break;
        case 0x1000E050:
            reg = RBOR;
            break;
        default:
            Logger::log(Logger::DMAC, "Unrecognized read32 from $%08X\n", address);
            break;
    }
    return reg;
}

void DMAC::write8(uint32_t address, uint8_t value)
{
    switch (address)
    {
        case 0x10009000:
            channels[VIF1].control &= ~0xFF;
            channels[VIF1].control |= value;
            break;
        default:
            Logger::log(Logger::DMAC, "Unrecognized write8 to $%08X of $%02X\n", address, value);
            break;
    }
}

void DMAC::write32(uint32_t address, uint32_t value)
{
    switch (address)
    {
        case 0x10008000:
            Logger::log(Logger::DMAC, "VIF0 CTRL: $%08X\n", value);
            channels[VIF0].control = value;
            if (value & 0x100)
                start_DMA(VIF0);
            break;
        case 0x10008010:
            Logger::log(Logger::DMAC, "VIF0 M_ADR: $%08X\n", value);
            channels[VIF0].address = value & ~0xF;
            break;
        case 0x10008020:
            Logger::log(Logger::DMAC, "VIF0 QWC: $%08X\n", value);
            channels[VIF0].quadword_count = value & 0xFFFF;
            break;
        case 0x10008030:
            Logger::log(Logger::DMAC, "VIF0 T_ADR: $%08X\n", value);
            channels[VIF0].tag_address = value & ~0xF;
            break;
        case 0x10009000:
            Logger::log(Logger::DMAC, "VIF1 CTRL: $%08X\n", value);
            channels[VIF1].control = value;
            if (value & 0x100)
                start_DMA(VIF1);
            break;
        case 0x10009010:
            Logger::log(Logger::DMAC, "VIF1 M_ADR: $%08X\n", value);
            channels[VIF1].address = value & ~0xF;
            break;
        case 0x10009020:
            Logger::log(Logger::DMAC, "VIF1 QWC: $%08X\n", value);
            channels[VIF1].quadword_count = value & 0xFFFF;
            break;
        case 0x10009030:
            Logger::log(Logger::DMAC, "VIF1 T_ADR: $%08X\n", value);
            channels[VIF1].tag_address = value & ~0xF;
            break;
        case 0x1000A000:
            Logger::log(Logger::DMAC, "GIF CTRL: $%08X\n", value);
            channels[GIF].control = value;
            if (value & 0x100)
            {
                start_DMA(GIF);
                gif->activate_PATH(3);
            }
            break;
        case 0x1000A010:
            Logger::log(Logger::DMAC, "GIF M_ADR: $%08X\n", value);
            channels[GIF].address = value & ~0xF;
            break;
        case 0x1000A020:
            Logger::log(Logger::DMAC, "GIF QWC: $%08X\n", value & 0xFFFF);
            channels[GIF].quadword_count = value & 0xFFFF;
            break;
        case 0x1000A030:
            Logger::log(Logger::DMAC, "GIF T_ADR: $%08X\n", value);
            channels[GIF].tag_address = value & ~0xF;
            break;
        case 0x1000B000:
            Logger::log(Logger::DMAC, "IPU_FROM CTRL: $%08X\n", value);
            channels[IPU_FROM].control = value;
            if (value & 0x100)
                start_DMA(IPU_FROM);
            break;
        case 0x1000B010:
            Logger::log(Logger::DMAC, "IPU_FROM M_ADR: $%08X\n", value);
            channels[IPU_FROM].address = value & ~0xF;
            break;
        case 0x1000B020:
            Logger::log(Logger::DMAC, "IPU_FROM QWC: $%08X\n", value);
            channels[IPU_FROM].quadword_count = value & 0xFFFF;
            break;
        case 0x1000B400:
            Logger::log(Logger::DMAC, "IPU_TO CTRL: $%08X\n", value);
            channels[IPU_TO].control = value;
            if (value & 0x100)
                start_DMA(IPU_TO);
            break;
        case 0x1000B410:
            Logger::log(Logger::DMAC, "IPU_TO M_ADR: $%08X\n", value);
            channels[IPU_TO].address = value & ~0xF;
            break;
        case 0x1000B420:
            Logger::log(Logger::DMAC, "IPU_TO QWC: $%08X\n", value);
            channels[IPU_TO].quadword_count = value & 0xFFFF;
            break;
        case 0x1000B430:
            Logger::log(Logger::DMAC, "IPU_TO T_ADR: $%08X\n", value);
            channels[IPU_TO].tag_address = value & ~0xF;
            break;
        case 0x1000C000:
            Logger::log(Logger::DMAC, "SIF0 CTRL: $%08X\n", value);
            channels[SIF0].control = value;
            if (value & 0x100)
                start_DMA(SIF0);
            break;
        case 0x1000C020:
            Logger::log(Logger::DMAC, "SIF0 QWC: $%08X\n", value);
            channels[SIF0].quadword_count = value & 0xFFFF;
            break;
        case 0x1000C030:
            Logger::log(Logger::DMAC, "SIF0 T_ADR: $%08X\n", value);
            channels[SIF0].tag_address = value & ~0xF;
            break;
        case 0x1000C400:
            Logger::log(Logger::DMAC, "SIF1 CTRL: $%08X\n", value);
            channels[SIF1].control = value;
            if (value & 0x100)
                start_DMA(SIF1);
            break;
        case 0x1000C410:
            Logger::log(Logger::DMAC, "SIF1 M_ADR: $%08X\n", value);
            channels[SIF1].address = value & ~0xF;
            break;
        case 0x1000C420:
            Logger::log(Logger::DMAC, "SIF1 QWC: $%08X\n", value);
            channels[SIF1].quadword_count = value & 0xFFFF;
            break;
        case 0x1000C430:
            Logger::log(Logger::DMAC, "SIF1 T_ADR: $%08X\n", value);
            channels[SIF1].tag_address = value & ~0xF;
            break;
        case 0x1000D000:
            Logger::log(Logger::DMAC, "SPR_FROM CTRL: $%08X\n", value);
            channels[SPR_FROM].control = value;
            if (value & 0x100)
                start_DMA(SPR_FROM);
            break;
        case 0x1000D010:
            Logger::log(Logger::DMAC, "SPR_FROM M_ADR: $%08X\n", value);
            channels[SPR_FROM].address = value & ~0xF;
            break;
        case 0x1000D020:
            Logger::log(Logger::DMAC, "SPR_FROM QWC: $%08X\n", value);
            channels[SPR_FROM].quadword_count = value & 0xFFFF;
            break;
        case 0x1000D080:
            Logger::log(Logger::DMAC, "SPR_FROM SADR: $%08X\n", value);
            channels[SPR_FROM].scratchpad_address = value & 0x3FFC;
            break;
        case 0x1000D400:
            Logger::log(Logger::DMAC, "SPR_TO CTRL: $%08X\n", value);
            channels[SPR_TO].control = value;
            if (value & 0x100)
                start_DMA(SPR_TO);
            break;
        case 0x1000D410:
            Logger::log(Logger::DMAC, "SPR_TO M_ADR: $%08X\n", value);
            channels[SPR_TO].address = value & ~0xF;
            break;
        case 0x1000D420:
            Logger::log(Logger::DMAC, "SPR_TO QWC: $%08X\n", value);
            channels[SPR_TO].quadword_count = value & 0xFFFF;
            break;
        case 0x1000D430:
            Logger::log(Logger::DMAC, "SPR_TO T_ADR: $%08X\n", value);
            channels[SPR_TO].tag_address = value & ~0xF;
            break;
        case 0x1000D480:
            Logger::log(Logger::DMAC, "SPR_TO SADR: $%08X\n", value);
            channels[SPR_TO].scratchpad_address = value & 0x3FFC;
            break;
        case 0x1000E000:
            Logger::log(Logger::DMAC, "Write32 D_CTRL: $%08X\n", value);
            control.master_enable = value & 0x1;
            control.cycle_stealing = value & 0x2;
            control.mem_drain_channel = (value >> 2) & 0x3;
            control.stall_source_channel = (value >> 4) & 0x3;
            control.stall_dest_channel = (value >> 6) & 0x3;
            control.release_cycle = (value >> 8) & 0x7;
            break;
        case 0x1000E010:
            Logger::log(Logger::DMAC, "Write32 D_STAT: $%08X\n", value);
            for (int i = 0; i < 15; i++)
            {
                if (value & (1 << i))
                    interrupt_stat.channel_stat[i] = false;

                //Reverse mask
                if (value & (1 << (i + 16)))
                    interrupt_stat.channel_mask[i] ^= 1;
            }
            int1_check();
            break;
        case 0x1000E020:
            Logger::log(Logger::DMAC, "Write to PCR: $%08X\n", value);
            PCR = value;
            break;
        case 0x1000E040:
            Logger::log(Logger::DMAC, "Write to RBSR: $%08X\n", value);
            RBSR = value;
            break;
        case 0x1000E050:
            Logger::log(Logger::DMAC, "Write to RBOR: $%08X\n", value);
            RBOR = value;
            break;
        default:
            Logger::log(Logger::DMAC, "Unrecognized write32 of $%08X to $%08X\n", value, address);
            break;
    }
}
