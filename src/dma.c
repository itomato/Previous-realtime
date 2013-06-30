/* NeXT DMA Emulation 
 * Contains informations from QEMU-NeXT
 * NeXT DMA consists of 12 channel processors with 128 bytes internal buffer for each channel
 * 12 channels: SCSI, Sound in, Sound out, Optical disk, Printer, SCC, DSP,
 * Ethernet transmit, Ethernet receive, Video, Memory to register, Register to memory
 */

#include "ioMem.h"
#include "ioMemTables.h"
#include "m68000.h"
#include "scsi.h"
#include "esp.h"
#include "sysReg.h"
#include "dma.h"
#include "configuration.h"
#include "ethernet.h"
#include "mmu_common.h"



#define LOG_DMA_LEVEL LOG_WARN

#define IO_SEG_MASK	0x1FFFF

/* DMA internal buffer size */
#define DMA_BURST_SIZE  16
int act_buf_size = 0;

/* read/write CSR bits */
#define DMA_M2DEV       0x00000000 /* dma from mem to dev */
#define DMA_DEV2M       0x00040000 /* dma from dev to mem */
/* read CSR bits */
#define DMA_ENABLE      0x01000000 /* enable dma transfer */
#define DMA_SUPDATE     0x02000000 /* single update */
#define DMA_COMPLETE    0x08000000 /* current dma has completed */
#define DMA_BUSEXC      0x10000000 /* bus exception occurred */
/* write CSR bits */
#define DMA_SETENABLE   0x00010000 /* set enable */
#define DMA_SETSUPDATE  0x00020000 /* set single update */
#define DMA_CLRCOMPLETE 0x00080000 /* clear complete conditional */
#define DMA_RESET       0x00100000 /* clr cmplt, sup, enable */
#define DMA_INITBUF     0x00200000 /* initialize DMA buffers */
/* CSR masks */
#define DMA_CMD_MASK    (DMA_SETENABLE|DMA_SETSUPDATE|DMA_CLRCOMPLETE|DMA_RESET|DMA_INITBUF)
#define DMA_STAT_MASK   (DMA_ENABLE|DMA_SUPDATE|DMA_COMPLETE|DMA_BUSEXC)


/* Read and write CSR bits for 68030 based NeXT Computer.
 * We convert these to 68040 values before using in functions.
 * read CSR bits *
 #define DMA_ENABLE      0x01
 #define DMA_SUPDATE     0x02
 #define DMA_COMPLETE    0x08
 #define DMA_BUSEXC      0x10
 * write CSR bits *
 #define DMA_SETENABLE   0x01
 #define DMA_SETSUPDATE  0x02
 #define DMA_M2DEV       0x00
 #define DMA_DEV2M       0x04
 #define DMA_CLRCOMPLETE 0x08
 #define DMA_RESET       0x10
 #define DMA_INITBUF     0x20
 */



static inline Uint32 dma_mklong(Uint8 *buf, Uint32 pos) {
	return (buf[pos] << 24) | (buf[pos+1] << 16) | (buf[pos+2] << 8) | buf[pos+3];
}


int get_channel(Uint32 address) {
    int channel = address&IO_SEG_MASK;

    switch (channel) {
        case 0x010: printf("channel SCSI:\n"); return CHANNEL_SCSI; break;
        case 0x040: printf("channel Sound Out:\n"); return CHANNEL_SOUNDOUT; break;
        case 0x050: printf("channel MO Disk:\n"); return CHANNEL_DISK; break;
        case 0x080: printf("channel Sound in:\n"); return CHANNEL_SOUNDIN; break;
        case 0x090: printf("channel Printer:\n"); return CHANNEL_PRINTER; break;
        case 0x0c0: printf("channel SCC:\n"); return CHANNEL_SCC; break;
        case 0x0d0: printf("channel DSP:\n"); return CHANNEL_DSP; break;
        case 0x110: printf("channel Ethernet Tx:\n"); return CHANNEL_EN_TX; break;
        case 0x150: printf("channel Ethernet Rx:\n"); return CHANNEL_EN_RX; break;
        case 0x180: printf("channel Video:\n"); return CHANNEL_VIDEO; break;
        case 0x1d0: printf("channel M2R:\n"); return CHANNEL_M2R; break;
        case 0x1c0: printf("channel R2M:\n"); return CHANNEL_R2M; break;
            
        default:
            Log_Printf(LOG_DMA_LEVEL, "Unknown DMA channel!\n");
            return -1;
            break;
    }
}

int get_interrupt_type(int channel) {
    switch (channel) {
        case CHANNEL_SCSI: return INT_SCSI_DMA; break;
        case CHANNEL_SOUNDOUT: return INT_SND_OUT_DMA; break;
        case CHANNEL_DISK: return INT_DISK_DMA; break;
        case CHANNEL_SOUNDIN: return INT_SND_IN_DMA; break;
        case CHANNEL_PRINTER: return INT_PRINTER_DMA; break;
        case CHANNEL_SCC: return INT_SCC_DMA; break;
        case CHANNEL_DSP: return INT_DSP_DMA; break;
        case CHANNEL_EN_TX: return INT_EN_TX_DMA; break;
        case CHANNEL_EN_RX: return INT_EN_RX_DMA; break;
        case CHANNEL_VIDEO: return 0; break;                   // no interrupt? CHECK THIS
        case CHANNEL_M2R: return INT_M2R_DMA; break;
        case CHANNEL_R2M: return INT_R2M_DMA; break;
                        
        default:
            Log_Printf(LOG_DMA_LEVEL, "Unknown DMA interrupt!\n");
            return 0;
            break;
    }
}

void DMA_CSR_Read(void) { // 0x02000010, length of register is byte on 68030 based NeXT Computer
    int channel = get_channel(IoAccessCurrentAddress);
    if(ConfigureParams.System.nMachineType == NEXT_CUBE030) { // for 68030 based NeXT Computer
        IoMem[IoAccessCurrentAddress & IO_SEG_MASK] = ((dma[channel].csr&DMA_STAT_MASK) >> 24) | ((dma[channel].csr&DMA_DEV2M) >> 16); /* status bits | direction bit */
        Log_Printf(LOG_DMA_LEVEL,"DMA CSR read at $%08x val=$%02x PC=$%08x\n", IoAccessCurrentAddress, dma[channel].csr >> 24, m68k_getpc());
    } else {
        IoMem_WriteLong(IoAccessCurrentAddress & IO_SEG_MASK, dma[channel].csr);
        Log_Printf(LOG_DMA_LEVEL,"DMA CSR read at $%08x val=$%08x PC=$%08x\n", IoAccessCurrentAddress, dma[channel].csr, m68k_getpc());
    }
}

void DMA_CSR_Write(void) {
    int channel = get_channel(IoAccessCurrentAddress);
    int interrupt = get_interrupt_type(channel);
    Uint32 writecsr;
    if(ConfigureParams.System.nMachineType == NEXT_CUBE030) { // for 68030 based NeXT Computer
        writecsr = IoMem[IoAccessCurrentAddress & IO_SEG_MASK] << 16;
        Log_Printf(LOG_DMA_LEVEL,"DMA CSR write at $%08x val=$%02x PC=$%08x\n", IoAccessCurrentAddress, writecsr >> 16, m68k_getpc());
    } else {
        writecsr = IoMem_ReadLong(IoAccessCurrentAddress & IO_SEG_MASK);
        Log_Printf(LOG_DMA_LEVEL,"DMA CSR write at $%08x val=$%08x PC=$%08x\n", IoAccessCurrentAddress, writecsr, m68k_getpc());
    }
     
    if(writecsr & DMA_DEV2M) {
        dma[channel].csr |= DMA_DEV2M;
        Log_Printf(LOG_DMA_LEVEL,"DMA from dev to mem");
    } else {
        dma[channel].csr &= ~DMA_DEV2M;
        Log_Printf(LOG_DMA_LEVEL,"DMA from mem to dev");
    }
    
    switch (writecsr&DMA_CMD_MASK) {
        case DMA_RESET:
            dma[channel].csr &= ~(DMA_COMPLETE | DMA_SUPDATE | DMA_ENABLE | DMA_DEV2M);
            Log_Printf(LOG_WARN,"DMA reset");
            break;
        case DMA_INITBUF:
            esp_dma.status = 0x00; /* just a guess */
            act_buf_size = 0;
            Log_Printf(LOG_DMA_LEVEL,"DMA initialize buffers");
            break;
        case (DMA_RESET | DMA_INITBUF):
            dma[channel].csr &= ~(DMA_COMPLETE | DMA_SUPDATE | DMA_ENABLE | DMA_DEV2M);
            esp_dma.status = 0x00; /* just a guess */
            act_buf_size = 0;
            Log_Printf(LOG_WARN,"DMA reset and initialize buffers");
            break;
        case DMA_CLRCOMPLETE:
            dma[channel].csr &= ~DMA_COMPLETE;
            Log_Printf(LOG_DMA_LEVEL,"DMA end chaining");
            if (channel==CHANNEL_SCSI) {
                dma_esp_write_memory();
            }
            break;
        case (DMA_SETSUPDATE | DMA_CLRCOMPLETE):
            dma[channel].csr |= DMA_SUPDATE;
            dma[channel].csr &= ~DMA_COMPLETE;
            Log_Printf(LOG_DMA_LEVEL,"DMA continue chaining");
            if (channel==CHANNEL_SCSI) {
                dma_esp_write_memory();
            }
            break;
        case DMA_SETENABLE:
            dma[channel].csr |= DMA_ENABLE;
            Log_Printf(LOG_DMA_LEVEL,"DMA start single transfer");
            break;
        case (DMA_SETENABLE | DMA_SETSUPDATE):
            dma[channel].csr |= DMA_SUPDATE;
            dma[channel].csr |= DMA_ENABLE;
            Log_Printf(LOG_DMA_LEVEL,"DMA start chaining");
            break;

        case 0x00:
            Log_Printf(LOG_WARN,"DMA no command");
            break;

        default:
            Log_Printf(LOG_DMA_LEVEL,"DMA: unknown command!");
            //abort();
            break;
    }
    
    set_interrupt(interrupt, RELEASE_INT); // experimental
}

void DMA_Saved_Next_Read(void) { // 0x02004000
    int channel = get_channel(IoAccessCurrentAddress-0x3FF0);
    IoMem_WriteLong(IoAccessCurrentAddress & IO_SEG_MASK, dma[channel].saved_next);
 	Log_Printf(LOG_DMA_LEVEL,"DMA SNext read at $%08x val=$%08x PC=$%08x\n", IoAccessCurrentAddress, dma[channel].saved_next, m68k_getpc());
}

void DMA_Saved_Next_Write(void) {
    int channel = get_channel(IoAccessCurrentAddress-0x3FF0);
    dma[channel].saved_next = IoMem_ReadLong(IoAccessCurrentAddress & IO_SEG_MASK);
    Log_Printf(LOG_DMA_LEVEL,"DMA SNext write at $%08x val=$%08x PC=$%08x\n", IoAccessCurrentAddress, dma[channel].saved_next, m68k_getpc());
}

void DMA_Saved_Limit_Read(void) { // 0x02004004
    int channel = get_channel(IoAccessCurrentAddress-0x3FF4);
    IoMem_WriteLong(IoAccessCurrentAddress & IO_SEG_MASK, dma[channel].saved_limit);
 	Log_Printf(LOG_DMA_LEVEL,"DMA SLimit read at $%08x val=$%08x PC=$%08x\n", IoAccessCurrentAddress, dma[channel].saved_limit, m68k_getpc());
}

void DMA_Saved_Limit_Write(void) {
    int channel = get_channel(IoAccessCurrentAddress-0x3FF4);
    dma[channel].saved_limit = IoMem_ReadLong(IoAccessCurrentAddress & IO_SEG_MASK);
    Log_Printf(LOG_DMA_LEVEL,"DMA SLimit write at $%08x val=$%08x PC=$%08x\n", IoAccessCurrentAddress, dma[channel].saved_limit, m68k_getpc());
}

void DMA_Saved_Start_Read(void) { // 0x02004008
    int channel = get_channel(IoAccessCurrentAddress-0x3FF8);
    IoMem_WriteLong(IoAccessCurrentAddress & IO_SEG_MASK, dma[channel].saved_start);
 	Log_Printf(LOG_DMA_LEVEL,"DMA SStart read at $%08x val=$%08x PC=$%08x\n", IoAccessCurrentAddress, dma[channel].saved_start, m68k_getpc());
}

void DMA_Saved_Start_Write(void) {
    int channel = get_channel(IoAccessCurrentAddress-0x3FF8);
    dma[channel].saved_start = IoMem_ReadLong(IoAccessCurrentAddress & IO_SEG_MASK);
    Log_Printf(LOG_DMA_LEVEL,"DMA SStart write at $%08x val=$%08x PC=$%08x\n", IoAccessCurrentAddress, dma[channel].saved_start, m68k_getpc());
}

void DMA_Saved_Stop_Read(void) { // 0x0200400c
    int channel = get_channel(IoAccessCurrentAddress-0x3FFC);
    IoMem_WriteLong(IoAccessCurrentAddress & IO_SEG_MASK, dma[channel].saved_stop);
 	Log_Printf(LOG_DMA_LEVEL,"DMA SStop read at $%08x val=$%08x PC=$%08x\n", IoAccessCurrentAddress, dma[channel].saved_stop, m68k_getpc());
}

void DMA_Saved_Stop_Write(void) {
    int channel = get_channel(IoAccessCurrentAddress-0x3FFC);
    dma[channel].saved_stop = IoMem_ReadLong(IoAccessCurrentAddress & IO_SEG_MASK);
    Log_Printf(LOG_DMA_LEVEL,"DMA SStop write at $%08x val=$%08x PC=$%08x\n", IoAccessCurrentAddress, dma[channel].saved_stop, m68k_getpc());
}

void DMA_Next_Read(void) { // 0x02004010
    int channel = get_channel(IoAccessCurrentAddress-0x4000);
    IoMem_WriteLong(IoAccessCurrentAddress & IO_SEG_MASK, dma[channel].next);
 	Log_Printf(LOG_DMA_LEVEL,"DMA Next read at $%08x val=$%08x PC=$%08x\n", IoAccessCurrentAddress, dma[channel].next, m68k_getpc());
}

void DMA_Next_Write(void) {
    int channel = get_channel(IoAccessCurrentAddress-0x4000);
    dma[channel].next = IoMem_ReadLong(IoAccessCurrentAddress & IO_SEG_MASK);
    Log_Printf(LOG_DMA_LEVEL,"DMA Next write at $%08x val=$%08x PC=$%08x\n", IoAccessCurrentAddress, dma[channel].next, m68k_getpc());
}

void DMA_Limit_Read(void) { // 0x02004014
    int channel = get_channel(IoAccessCurrentAddress-0x4004);
    IoMem_WriteLong(IoAccessCurrentAddress & IO_SEG_MASK, dma[channel].limit);
 	Log_Printf(LOG_DMA_LEVEL,"DMA Limit read at $%08x val=$%08x PC=$%08x\n", IoAccessCurrentAddress, dma[channel].limit, m68k_getpc());
}

void DMA_Limit_Write(void) {
    int channel = get_channel(IoAccessCurrentAddress-0x4004);
    dma[channel].limit = IoMem_ReadLong(IoAccessCurrentAddress & IO_SEG_MASK);
    Log_Printf(LOG_DMA_LEVEL,"DMA Limit write at $%08x val=$%08x PC=$%08x\n", IoAccessCurrentAddress, dma[channel].limit, m68k_getpc());
}

void DMA_Start_Read(void) { // 0x02004018
    int channel = get_channel(IoAccessCurrentAddress-0x4008);
    IoMem_WriteLong(IoAccessCurrentAddress & IO_SEG_MASK, dma[channel].start);
 	Log_Printf(LOG_DMA_LEVEL,"DMA Start read at $%08x val=$%08x PC=$%08x\n", IoAccessCurrentAddress, dma[channel].start, m68k_getpc());
}

void DMA_Start_Write(void) {
    int channel = get_channel(IoAccessCurrentAddress-0x4008);
    dma[channel].start = IoMem_ReadLong(IoAccessCurrentAddress & IO_SEG_MASK);
    Log_Printf(LOG_DMA_LEVEL,"DMA Start write at $%08x val=$%08x PC=$%08x\n", IoAccessCurrentAddress, dma[channel].start, m68k_getpc());
}

void DMA_Stop_Read(void) { // 0x0200401c
    int channel = get_channel(IoAccessCurrentAddress-0x400C);
    IoMem_WriteLong(IoAccessCurrentAddress & IO_SEG_MASK, dma[channel].stop);
 	Log_Printf(LOG_DMA_LEVEL,"DMA Stop read at $%08x val=$%08x PC=$%08x\n", IoAccessCurrentAddress, dma[channel].stop, m68k_getpc());
}

void DMA_Stop_Write(void) {
    int channel = get_channel(IoAccessCurrentAddress-0x400C);
    dma[channel].stop = IoMem_ReadLong(IoAccessCurrentAddress & IO_SEG_MASK);
    Log_Printf(LOG_DMA_LEVEL,"DMA Stop write at $%08x val=$%08x PC=$%08x\n", IoAccessCurrentAddress, dma[channel].stop, m68k_getpc());
}

void DMA_Init_Read(void) { // 0x02004210
    int channel = get_channel(IoAccessCurrentAddress-0x4200);
    IoMem_WriteLong(IoAccessCurrentAddress & IO_SEG_MASK, dma[channel].init);
 	Log_Printf(LOG_DMA_LEVEL,"DMA Init read at $%08x val=$%08x PC=$%08x\n", IoAccessCurrentAddress, dma[channel].init, m68k_getpc());
}

void DMA_Init_Write(void) {
    int channel = get_channel(IoAccessCurrentAddress-0x4200);
    dma[channel].next = dma[channel].init = IoMem_ReadLong(IoAccessCurrentAddress & IO_SEG_MASK); /* hack */
    Log_Printf(LOG_DMA_LEVEL,"DMA Init write at $%08x val=$%08x PC=$%08x\n", IoAccessCurrentAddress, dma[channel].init, m68k_getpc());
}

void DMA_Size_Read(void) { // 0x02004214
    int channel = get_channel(IoAccessCurrentAddress-0x4204);
    IoMem_WriteLong(IoAccessCurrentAddress & IO_SEG_MASK, dma[channel].size);
 	Log_Printf(LOG_DMA_LEVEL,"DMA Size read at $%08x val=$%08x PC=$%08x\n", IoAccessCurrentAddress, dma[channel].size, m68k_getpc());
}

void DMA_Size_Write(void) {
    int channel = get_channel(IoAccessCurrentAddress-0x4204);
    dma[channel].size = IoMem_ReadLong(IoAccessCurrentAddress & IO_SEG_MASK);
    Log_Printf(LOG_DMA_LEVEL,"DMA Size write at $%08x val=$%08x PC=$%08x\n", IoAccessCurrentAddress, dma[channel].size, m68k_getpc());
}



/* DMA Functions */


void dma_memory_read(Uint8 *buf, Uint32 *size, int channel) {
    Uint32 base_addr;
    Uint8 align = 16;
    Uint32 size_count = 0;
    Uint32 read_addr;
    int interrupt = get_interrupt_type(channel);
    
    if ((channel == CHANNEL_EN_TX) && !ConfigureParams.System.bTurbo)
        *size = (dma[channel].limit&0x0FFFFFFF) - (dma[channel].init&0x0FFFFFFF);
    else
        *size = (dma[channel].limit&0x0FFFFFFF) - (dma[channel].next&0x0FFFFFFF);
    
    if(channel == CHANNEL_EN_RX || channel == CHANNEL_EN_TX)
        align = 32;
    
//    if((*size % align) != 0) {
//        *size -= *size % align;
//        *size += align;
//    }
    
    if(dma[channel].init == 0)
        base_addr = dma[channel].next;
    else
        base_addr = dma[channel].init;
    
    Log_Printf(LOG_WARN, "[DMA] Read from mem: at $%08x, %i bytes",base_addr, *size);
    for (size_count = 0; size_count < *size; size_count++) {
        read_addr = base_addr + size_count;
        buf[size_count] = NEXTMemory_ReadByte(read_addr);
    }
    printf("READ FROM MEMORY: %02x\n", buf[0]);

    dma[channel].csr |= DMA_COMPLETE | DMA_SUPDATE;
    
    set_interrupt(interrupt, SET_INT);
}


void dma_esp_write_memory(void) {
    int i;
    int interrupt = get_interrupt_type(CHANNEL_SCSI);
    
    /* do we need to first save init, limit, start, stop to "saved" regs? */
        
    Log_Printf(LOG_WARN, "[DMA] Write to memory at $%08x, %i bytes",dma[CHANNEL_SCSI].next,esp_counter);
    
    if ((dma[CHANNEL_SCSI].limit-dma[CHANNEL_SCSI].next)%DMA_BURST_SIZE) {
        Log_Printf(LOG_WARN, "[DMA] Warning! DMA not aligned to burst size: Next = %08X, Limit = %08X, burst size = %i",dma[CHANNEL_SCSI].next,dma[CHANNEL_SCSI].limit, DMA_BURST_SIZE);
    }

    /* TODO: Find out how we should handle non burst-size aligned start address. 
     * End address is always burst-size aligned. */
    
    TRY(prb) {
        do {
            /* Fill DMA internal buffer (no real buffer, we use an imaginary one) */
            while (act_buf_size<DMA_BURST_SIZE && esp_counter>0 && scsi_phase==STAT_DI) {
                esp_counter--;
                SCSIdisk_Send_Data();
                act_buf_size++;
            }
            ESP_DMA_set_status();
            
            //Log_Printf(LOG_WARN, "[DMA] Internal buffer size: %i bytes",act_buf_size);
            
            /* If buffer is full, burst write to memory */
            if (act_buf_size==DMA_BURST_SIZE && dma[CHANNEL_SCSI].next<dma[CHANNEL_SCSI].limit) {
                for (i=0; i<act_buf_size; i+=4) {
                    NEXTMemory_WriteLong(dma[CHANNEL_SCSI].next+i, dma_mklong(SCSIdata.buffer, SCSIdata.rpos-act_buf_size+i));
                }
                dma[CHANNEL_SCSI].next+=act_buf_size;
                act_buf_size = 0;
            } else { /* else do not write the bytes to memory but keep them inside the buffer */ 
                Log_Printf(LOG_WARN, "[DMA] Residual bytes in DMA buffer: %i bytes",act_buf_size);
                break;
            }
        } while ((dma[CHANNEL_SCSI].limit-dma[CHANNEL_SCSI].next)>=DMA_BURST_SIZE);
    } CATCH(prb) {
        Log_Printf(LOG_WARN, "[DMA] Bus error while writing to %08x",dma[CHANNEL_SCSI].next+i);
        dma[CHANNEL_SCSI].csr &= ~DMA_ENABLE;
        dma[CHANNEL_SCSI].csr |= (DMA_COMPLETE|DMA_BUSEXC);
    } ENDTRY
    
    
    /* If we have reached limit, generate an interrupt and set the flags */
    if (dma[CHANNEL_SCSI].next==dma[CHANNEL_SCSI].limit) {
        set_interrupt(interrupt, SET_INT);
        dma[CHANNEL_SCSI].csr |= DMA_COMPLETE;

        if(dma[CHANNEL_SCSI].csr & DMA_SUPDATE) { /* if we are in chaining mode */
            dma[CHANNEL_SCSI].next = dma[CHANNEL_SCSI].start;
            dma[CHANNEL_SCSI].limit = dma[CHANNEL_SCSI].stop;
            /* Set bits in CSR */
            dma[CHANNEL_SCSI].csr &= ~DMA_SUPDATE; /* 1st done */
        } else {
            dma[CHANNEL_SCSI].csr &= ~DMA_ENABLE; /* all done */
//            esp_dma_done(); /* Give control back to ESP */
        }
//        return;
    }
    
    /* If ESP counter reached zero, give control to ESP */
//    if (esp_counter==0) {
        esp_dma_done(true);
//    }
}

void dma_esp_flush_buffer(void) {
//    int interrupt = get_interrupt_type(CHANNEL_SCSI);

    TRY(prb) {
        if (dma[CHANNEL_SCSI].next<dma[CHANNEL_SCSI].limit) {
            Log_Printf(LOG_WARN, "[DMA] Flush buffer to memory at $%08x, 4 bytes",dma[CHANNEL_SCSI].next);

            /* Write one long word to memory */
            NEXTMemory_WriteLong(dma[CHANNEL_SCSI].next, dma_mklong(SCSIdata.buffer, SCSIdata.rpos-act_buf_size));

            dma[CHANNEL_SCSI].next += 4;
            /* TODO: Check if we should also change rpos if act_buf_size is exceeded 
             * to write correct data from SCSI buffer. */
            if (act_buf_size>4) {
                act_buf_size -= 4;
            } else {
                act_buf_size = 0;
            }
        }
    } CATCH(prb) {
        dma[CHANNEL_SCSI].csr &= ~DMA_ENABLE;
        dma[CHANNEL_SCSI].csr |= (DMA_COMPLETE|DMA_BUSEXC);
    } ENDTRY
    
    /* Set interrupt if limit is reached (not sure this is needed) */
//    if (dma[CHANNEL_SCSI].next==dma[CHANNEL_SCSI].limit) {
//        set_interrupt(interrupt, SET_INT);
//    }
}

void dma_memory_write(Uint8 *buf, Uint32 size, int channel) {
    Uint32 base_addr, tail_addr;
    Uint8 align = 16;
    Uint32 size_count = 0;
    Uint32 write_addr;
    Uint32 dma_tail = 0;
    int interrupt = get_interrupt_type(channel);
    
    if(channel == CHANNEL_EN_RX || channel == CHANNEL_EN_TX)
        align = 32;
        
//    if((size % align) != 0) {
//        size -= size % align;
//        size += align;
//    }

    
    if(dma[channel].init == 0) {
        base_addr = dma[channel].next;
    } else {
        base_addr = dma[channel].init;
        
        /* If the transfer size is greater than (limit - init):
         * Copy residual bytes to physical addresses at start. */
        if (size > (dma[channel].limit - dma[channel].init)) {
            tail_addr = dma[channel].start;
            dma_tail = size - (dma[channel].limit - dma[channel].init);
            size = (dma[channel].limit - dma[channel].init);
            Log_Printf(LOG_WARN, "[DMA] Residual bytes: %i", dma_tail);
        }
    }

    Log_Printf(LOG_WARN, "[DMA] Write to mem: at $%08x, %i bytes",base_addr,size);
    for (size_count = 0; size_count < size; size_count++) {
        write_addr = base_addr + size_count;
        NEXTMemory_WriteByte(write_addr, buf[size_count]);
    }
    
    /* If there are residual bytes, copy them to physical addresses starting
     * at "start". */
    
    if (dma_tail) {
        Log_Printf(LOG_WARN, "[DMA] Write residual bytes at $%08x, %i bytes",tail_addr,dma_tail);
        for (size_count = 0; size_count < dma_tail; size_count++) {
            write_addr = tail_addr + size_count;
            NEXTMemory_WriteByte(write_addr, buf[size+size_count]);
        }
    }

    
    /* Test read/write */
    Log_Printf(LOG_DMA_LEVEL, "DMA Write Test: $%02x,$%02x,$%02x,$%02x\n", NEXTMemory_ReadByte(base_addr),NEXTMemory_ReadByte(base_addr+16),NEXTMemory_ReadByte(base_addr+32),NEXTMemory_ReadByte(base_addr+384));
//    NEXTMemory_WriteByte(base_addr, 0x77);
//    Uint8 testvar = NEXTMemory_ReadByte(base_addr);
//    Log_Printf(LOG_DMA_LEVEL, "Write Test: $%02x at $%08x", testvar, base_addr);
    
    dma[channel].init = 0;
    
    /* saved limit is checked to calculate packet size
     by both the rom and netbsd */ 
    dma[channel].saved_limit = dma[channel].next + size;
    dma[channel].saved_next  = dma[channel].next;
    
    if(!(dma[channel].csr & DMA_SUPDATE)||(channel==CHANNEL_EN_RX)) { // Ethernet: this needs to be checked!
        dma[channel].next = dma[channel].start;
        dma[channel].limit = dma[channel].stop;
    }

    dma[channel].csr |= DMA_COMPLETE;
    
    set_interrupt(interrupt, SET_INT);
//    set_interrupt(INT_SCSI_DMA, RELEASE_INT);
}
