//  MZ-1500 Multi-function RAMFILE card 'Niseramfile'
//
//  MZ-1R12: RAM Memory (NO Battery backup)
//  MZ-1R18: RAMFILE
//  MZ-1R23: Kanji ROM
//  MZ-1R24: Jisho ROM
//  PIO-3034: EMM

//  MZ-1500 extention slot
//  GP0-7:  D0-7
//  GP8-23: A0-15
//  GP25: RESET -> Interrupt
//  GP28: MERQ
//  GP29: IORQ
//  GP30: WR
//  GP31: RD
//  GP32: EXWAIT

#define FLASH_INTERVAL 300      // 5sec

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "pico/sync.h"
#include "hardware/pio.h"
#include "hardware/clocks.h"
#include "hardware/timer.h"
#include "hardware/uart.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "hardware/vreg.h"

#include "vga16_graphics.h"

#include "misc.h"

#define RESET_PIN 25

// RAM configuration
// MZ-1R18: 64KiB
// MZ-1R12: 32KiB
// EMM:    320KiB

uint8_t __attribute__  ((aligned(sizeof(unsigned char *)*4096))) mz1r12[0x10000];
uint8_t __attribute__  ((aligned(sizeof(unsigned char *)*4096))) mz1r18[0x10000];
uint8_t __attribute__  ((aligned(sizeof(unsigned char *)*4096))) emm[0x50000];

// PCG emulation (36KiB)
// VRAM  4KiB
// CGRAM 4Kib
// PCG   24KiB

uint8_t vram[0x1000];
uint8_t cgram[0x1000];
uint8_t pcg[0x2000 * 3];
uint8_t pcg700[0x800];

volatile uint8_t vram_enabled;
volatile uint8_t pcg_enabled;

#define MAXROMPAGE 64       // =  32KiB * 64 pages =  2MiB
#define MAXEMMPAGE 32       // = 320KiB * 32 pages = 10MiB

volatile uint8_t rompage=0;  // Page No of MZ-1R12 (64KiB/page)
volatile uint8_t emmpage=0;  // Page No of EMM (320KiB/page)

volatile uint32_t kanjiptr=0;
volatile uint32_t dictptr=0;
volatile uint32_t kanjictl=0;
volatile uint32_t mz1r18ptr=0;
volatile uint32_t mz1r12ptr=0;
volatile uint32_t emmptr=0;
volatile uint32_t flash_command=0;
volatile uint32_t pcg700_ptr=0;

// ROM configuration
// WeACT RP2350B with 16MiB Flash
// FONT ROM         4KiB   @ 0x1001f000
// Kanji ROM        128KiB @ 0x10020000
// Jisho ROM        256KiB @ 0x10040000
// Data for 1R12    32KiB  * 64 = 2MiB  @ 0x10080000
// Data for EMM     320KiB * 32 = 10MiB @ 0x10280000

#define ROMBASE 0x10080000u

uint8_t *fontrom=(uint8_t *)(ROMBASE-0x61000);
uint8_t *kanjirom=(uint8_t *)(ROMBASE-0x60000);
uint8_t *jishorom=(uint8_t *)(ROMBASE-0x40000);
uint8_t *romslots=(uint8_t *)(ROMBASE);
uint8_t *emmslots=(uint8_t *)(ROMBASE+(0x8000*MAXROMPAGE));

// VGA Output
uint8_t scandata[320];
extern unsigned char vga_data_array[];
volatile uint32_t video_hsync,video_vsync,scanline,vsync_scanline;
volatile uint8_t pcg_control;
volatile uint8_t pcg700_control,pcg700_data;
uint8_t pallet[8];

uint8_t colors[16]; // DUMMY

//uint8_t __attribute__  ((aligned(sizeof(unsigned char *)*4096))) flash_buffer[4096];

void __not_in_flash_func(mzscan)(uint8_t scan);

// *REAL* H-Sync for emulation
void __not_in_flash_func(hsync_handler)(void) {

    uint32_t vramindex;
    uint32_t tmsscan;
    uint8_t bgcolor;

    pio_interrupt_clear(pio0, 0);

    if((scanline!=0)&&(gpio_get(VSYNC)==0)) { // VSYNC
        scanline=0;
        video_vsync=1;
    } else {
        scanline++;
    }

    if((scanline%2)==0) {
        video_hsync=1;

        // VDP Draw on HSYNC

        // VGA Active starts scanline 35
        //          Active scanline 78(0) to 477(199)

        if((scanline>=73)&&(scanline<=472)) {
//        if((scanline>=81)&&(scanline<=480)) {

            tmsscan=(scanline-73)/2;
//            tmsscan=(scanline-81)/2;
            vramindex=(tmsscan%4)*320;

            mzscan(tmsscan);

            for(int j=0;j<320;j++) {
                vga_data_array[vramindex+j]=scandata[j];
            }           
        }

    }

    return;

}

//
//  reset

void __not_in_flash_func(z80reset)(uint gpio,uint32_t event) {

//    gpio_acknowledge_irq(RESET_PIN,GPIO_IRQ_EDGE_FALL);
    gpio_acknowledge_irq(RESET_PIN,GPIO_IRQ_EDGE_RISE);

    if(gpio_get(RESET_PIN)==0) return;

    kanjiptr=0;
    dictptr=0;
    kanjictl=0;
    mz1r18ptr=0;
    mz1r12ptr=0;
    emmptr=0;
    pcg_control=0;
    pcg700_ptr=0;
    pcg700_control=0xff;

    vram_enabled=1;
    pcg_enabled=0xff;

    return;
}

// VGA Out

void __not_in_flash_func(mzscan)(uint8_t scan) {

    uint8_t scanx,scany,scanyy;
    uint8_t ch,color,font,pcgfontb,pcgfontr,pcgfontg,fgcolor,bgcolor;
    uint16_t pcgch;
    uint32_t offset;
    uint8_t scan_buffer[8];

    union bytemember {
         uint32_t w;
         uint8_t b[4];
    };

    union bytemember bitf1,bitf2,bitb1,bitb2,bitp1,bitp2;

    scany=scan/8;
    scanyy=scan%8;
    offset=scany*40;

    for(scanx=0;scanx<40;scanx++) {

        // Charactor data
        ch=vram[offset];
        color=vram[offset+0x800];

        if(((pcg700_control&8)==0)&&(ch>0x7f)) {
            if(color&0x80) {
                font=pcg700[(ch&0x7f)*8+scanyy+0x400];
            } else {
                font=pcg700[(ch&0x7f)*8+scanyy];
            }
        } else {
            if(color&0x80) {
                font=cgram[ch*8+scanyy+0x800];
            } else {
                font=cgram[ch*8+scanyy];
            }
        }

        fgcolor=(color&0x70)>>4;
        bgcolor=color&7;

        bitf1.w=bitexpand[font*4  ]*fgcolor;
        bitf2.w=bitexpand[font*4+1]*fgcolor;

        bitb1.w=bitexpand[font*4+2]*bgcolor;
        bitb2.w=bitexpand[font*4+3]*bgcolor;

        // PCG data
        if((pcg_control&1) && (vram[offset+0xc00]&8)) {

            pcgch=vram[offset+0x400]+((uint16_t)vram[offset+0xc00]&0xc0)*4;

            pcgfontb=pcg[pcgch*8+scanyy];
            pcgfontr=pcg[pcgch*8+scanyy+0x2000];
            pcgfontg=pcg[pcgch*8+scanyy+0x4000];

            bitp1.w=bitexpand[pcgfontb*4  ]+bitexpand[pcgfontr*4  ]*2+bitexpand[pcgfontg*4  ]*4;
            bitp2.w=bitexpand[pcgfontb*4+1]+bitexpand[pcgfontr*4+1]*2+bitexpand[pcgfontg*4+1]*4;

        } else {
            bitp1.w=0;
            bitp2.w=0;
        }
        
        // Merge
        if(pcg_control&2) {
            // PCG > TEXT
            for(int i=0;i<4;i++) {
                if(bitp1.b[i]!=0) {
                    scan_buffer[i+4]=bitp1.b[i];
                } else {
                    scan_buffer[i+4]=bitf1.b[i]+bitb1.b[i];
                }
                if(bitp2.b[i]!=0) {
                    scan_buffer[i]=bitp2.b[i];
                } else {
                    scan_buffer[i]=bitf2.b[i]+bitb2.b[i];
                }                        
            }
        } else {
            // TEXT > PCG
            for(int i=0;i<4;i++) {
                if(bitf1.b[i]!=0) {
                    scan_buffer[i+4]=bitf1.b[i];
                } else if(bitp1.b[i]!=0) {
                    scan_buffer[i+4]=bitp1.b[i];
                } else {
                    scan_buffer[i+4]=bitb1.b[i];
                }
                if(bitf2.b[i]!=0) {
                    scan_buffer[i]=bitf2.b[i];
                } else if(bitp2.b[i]!=0) {
                    scan_buffer[i]=bitp2.b[i];
                } else {
                    scan_buffer[i]=bitb2.b[i];
                }
            }
        }

    // Color pallet

        for(int i=0;i<8;i++) {
                      scandata[scanx*8+i]=pallet[scan_buffer[i]];     
        }

        offset++;

    }

    return;

}


static inline void io_write(uint16_t address, uint8_t data)
{

    uint8_t b;

    switch(address&0xff) {

        // MZ-1R23 & 1R24

        case 0xb8:  // Kanji Control
            kanjictl=data;
            return;

        case 0xb9:  // Kanji PTR
            dictptr=data+(address&0xff00);
            kanjiptr=dictptr<<5;

            return;

        // MZ-1R18

        case 0xea:  // RAMFILE WRITE 
            mz1r18[mz1r18ptr&0xffff]=data;
            mz1r18ptr++;
            return;

        case 0xeb:  // RAMFILE PTR
            mz1r18ptr=data+(address&0xff00);
            return;
        
        // MZ-1R12

        case 0xf8:  // MZ-1R12 ptr high 
            mz1r12ptr=(data<<8)+(mz1r12ptr&0xff);
            mz1r12ptr&=0x7fff;
            return;

        case 0xf9:  // MZ-1R12 ptr low
            mz1r12ptr=data+(mz1r12ptr&0xff00);
            return;

        case 0xfa:
//            mz1r12[mz1r12ptr&0xffff]=data;
            mz1r12[mz1r12ptr&0x7fff]=data;
            mz1r12ptr++;
            if(mz1r12ptr>0x7fff) { 
                mz1r12ptr=0;
            }
            return;

        //  MZ-1R12 control 

        case 0xba:
            rompage=data&0x3f;
            flash_command=0x10000000+(data&0x3f);
            return;

        case 0xbb:
            rompage=data&0x3f;
            flash_command=0x20000000+(data&0x3f);
            return;

        case 0xbc:
            emmpage=data&0x1f;
            flash_command=0x30000000+(data&0x1f);        
            return;

        case 0xbd:
            emmpage=data&0x1f;
            flash_command=0x40000000+(data&0x1f);        
            return;

        // EMM
        case 0:
            emmptr=(emmptr&0x7ff00)+(data);
            return;

        case 1:
            emmptr=(emmptr&0x700ff)+(data<<8);
            return;           

        case 2:
//            emmptr=(emmptr&0xffff)+(data<<16);
            emmptr=(emmptr&0xffff)+((data&7)<<16);
            return; 

        case 3:
            if(emmptr>0x4ffff) {
                emmptr-=0x50000;
            }
            emm[emmptr++]=data;
            return;

        // BANK control

        case 0xe1:  // BANK TO DRAM
            vram_enabled=0;
            return;

        case 0xe3:  // BANK TO VRAM
            vram_enabled=1;
            return;

        case 0xe4:
            vram_enabled=1;
            pcg_enabled=0xff;
            return;

        case 0xe5:  // BANK TO PCG
            pcg_enabled=data&3;
            return;

        case 0xe6:  // BANK FROM PCG
            pcg_enabled=0xff;
            return;

        // VGA Out

        case 0xf0:
            pcg_control=data;
            return;

        case 0xf1:
            pallet[(data&0x70)>>4]=data&7;
            return;

    }
 
    return;

}

static inline void memory_write(uint16_t address, uint8_t data)
{

    if(address<0xd000) { // Main RAM
        return;
    }

    if((pcg_enabled!=0xff)&&(address<0xf000)) {

        if((pcg_enabled&3)!=0) {
            // PCG BANK
            pcg[(address-0xd000)+((pcg_enabled&3)-1)*0x2000]=data;

        }

    } else if((vram_enabled)&&(address<0xe000)) {

            vram[address&0xfff]=data;

    } else {

        if(vram_enabled) {

        switch(address) {

            // PCG 700

            case 0xe010:
                pcg700_data=data;
                return;

            case 0xe011:
                pcg700_ptr&=0x700;
                pcg700_ptr|=data;
                return;

            case 0xe012:
                if(pcg700_control&0x10) { // WE: 1 -> 0
                    if((data&0x10)==0) {
                        // write operaton

                        if(pcg700_control&0x20) {
                            // COPY CGROM
                            if(pcg700_ptr<0x400) {
                                pcg700[pcg700_ptr]=cgram[pcg700_ptr+0x400];
                            } else {
                                pcg700[pcg700_ptr]=cgram[pcg700_ptr+0x800];                                
                            }
                        } else {
                            pcg700[pcg700_ptr]=pcg700_data;
                        }
                    }
                } else {    // WE: 0 -> 1
                    if(data&0x10) {
                        // set high address
                        pcg700_ptr&=0xff;
                        pcg700_ptr|=(data&7)<<8;
                    }
                }
                pcg700_control=data;
                return;
        }

        }

    }

    return;

}


void init_emulator(void) {

    // Erase Gamepad info

    kanjiptr=0;
    mz1r12ptr=0;
    mz1r18ptr=0;
    emmptr=0;
    pcg700_ptr=0;
    pcg700_control=0xff;
    pcg_control=0;

    vram_enabled=1;
    pcg_enabled=0xff;

    for(int i=0;i<8;i++) {
        pallet[i]=i;
    }

    memcpy(cgram,fontrom,0x1000);

    // Load DUMMY data to MZ-1R12

//    memcpy(mz1r12,romslots,0x8000);
    memset(mz1r12,0xaa,0x8000);

    // Load EMM data to EMM

    memcpy(emm,emmslots,0x50000);

}

// Main thread (Core1)

void __not_in_flash_func(main_core1)(void) {

    volatile uint32_t bus;

    uint32_t control,address,data,response;
    uint32_t needwait=0;

//    multicore_lockout_victim_init();

    gpio_init_mask(0xffffffff);
    gpio_set_dir_all_bits(0x00000000);  // All pins are INPUT
//    gpio_set_dir(32,true);

    while(1) {

        bus=gpio_get_all();

        control=bus&0xf0000000;

        // Check IO Read

        if(control==0x50000000) {

            address=(bus&0xffff00)>>8;

            switch(address&0xff) {

                // MZ-1R23 & 1R24

                case 0xb9:
                    // ENABLE EXWAIT
                    gpio_set_dir(32,true);
                    gpio_put(32,false);
                    needwait=1;

                    if(kanjictl&0x80) {
                        // KANJI
                        data=kanjirom[kanjiptr++];
                        if(kanjictl&0x40) { // Bit reverse
                            data=bitreverse[data];           
                        }
                        response=1;
                        break;

                    } else {
                        data=jishorom[((kanjictl&3)<<16)+(dictptr++)];
                        if(kanjictl&0x40) { // Bit reverse
                            data=bitreverse[data];           
                        }
                        response=1;
                        break;
                    }

                // MZ-1R18

                case 0xea:
                    data=mz1r18[mz1r18ptr&0xffff];
                    mz1r18ptr++;
                    response=1;
                    break;

// DEBUG

                case 0xeb:
                    data=mz1r18ptr&0xff;
                    response=1;
                    break;


                // MZ-1R12

                case 0xf8:
                    mz1r12ptr=0;
                    data=0;
                    response=1;
                    break;

                case 0xf9:
//                    data=mz1r12[mz1r12ptr&0xffff];
                    data=mz1r12[mz1r12ptr&0x7fff];
                    mz1r12ptr++;
                    if(mz1r12ptr>0x7fff) {
                        mz1r12ptr=0;
                    }
                    response=1;
                    break;

                // EMM
                case 0x3:
                    if(emmptr>0x4ffff) {
                        emmptr-=0x50000;
                    }
                    data=emm[emmptr++];
                    response=1;
                    break;

                default:
                    response=0;

            }

            if(response) {

                if(needwait) {
                    gpio_put(32,true);
                    gpio_set_dir(32,false);
                    needwait=0;
                }

                // Set GP0-7 to OUTPUT

                gpio_set_dir_masked(0xff,0xff);

                gpio_put_masked(0xff,data);

                // Wait while RD# is low

                control=0;

                while(control==0) {
                    bus=gpio_get_all();
                    control=bus&0x80000000;
                }

                // Set GP0-7 to INPUT

                gpio_set_dir_masked(0xff,0x00);

            } else {

                // Wait while RD# is low
                control=0;

                while(control==0) {
                    bus=gpio_get_all();
                    control=bus&0x80000000;
                }

            }

            continue;
        }

        // Check IO Write

        else if(control==0x90000000) {

            address=(bus&0xffff00)>>8;
            data=bus&0xff;

            io_write(address,data);

            control=0;

            // Wait while WR# is low
            while(control==0) {
                bus=gpio_get_all();
                control=bus&0x40000000;
            }

            continue;
        }
        
        // check Memory write

        else if(control==0xa0000000) {

            address=(bus&0xffff00)>>8;
            data=bus&0xff;

            memory_write(address,data);

            control=0;

            // Wait while WR# is low
            while(control==0) {
                bus=gpio_get_all();
                control=bus&0x40000000;
            }


        }

    }
}

int main() {

    uint32_t menuprint=0;
    uint32_t filelist=0;
    uint32_t subcpu_wait;
    uint32_t rampacno;
    uint32_t pacpage;

    static uint32_t hsync_wait,vsync_wait;

    vreg_set_voltage(VREG_VOLTAGE_1_20);  // for overclock to 300MHz
    set_sys_clock_khz(300000 ,true);

    initVGA();

    multicore_launch_core1(main_core1);

    init_emulator();

    irq_set_exclusive_handler (PIO0_IRQ_0, hsync_handler);
    irq_set_enabled(PIO0_IRQ_0, true);
    pio_set_irq0_source_enabled (pio0, pis_interrupt0 , true);

    // Set RESET# interrupt

//    gpio_set_irq_enabled_with_callback(RESET_PIN,GPIO_IRQ_EDGE_FALL,true,z80reset);
    gpio_set_irq_enabled_with_callback(RESET_PIN,GPIO_IRQ_EDGE_RISE,true,z80reset);

    while(1) {

        if(flash_command!=0) {

            switch(flash_command&0xf0000000) {

                case 0x10000000:  // SRAM load
                    memcpy(mz1r12,romslots+(flash_command&0x3f)*0x8000u,0x8000);
                    break;

                case 0x20000000:  // SRAM Save

                    for(uint32_t i=0;i<0x8000;i+=4096) {
                    uint32_t ints = save_and_disable_interrupts();
//                    multicore_lockout_start_blocking();     // pause another core
                    flash_range_erase(i+0x80000u+(flash_command&0x3f)*0x8000u, 4096);  
//                    multicore_lockout_end_blocking();
                    restore_interrupts(ints);
                    }

                    for(uint32_t i=0;i<0x8000;i+=4096) {
                    uint32_t ints = save_and_disable_interrupts();
//                    multicore_lockout_start_blocking();     // pause another core
                    flash_range_program(i+0x80000u+(flash_command&0x3f)*0x8000u, (const uint8_t *)(mz1r12+i), 4096);
//                    multicore_lockout_end_blocking();
                    restore_interrupts(ints);
                    }

                    break;

                case 0x30000000:  // EMM load
                    memcpy(emm,emmslots+(flash_command&0x1f)*0x50000u,0x50000);
                    break;                    

                case 0x40000000:  // EMM Save

                    for(uint32_t i=0;i<0x50000;i+=4096) {
                    uint32_t ints = save_and_disable_interrupts();
//                    multicore_lockout_start_blocking();     // pause another core
                    flash_range_erase(i+0x280000u+(flash_command&0x1f)*0x50000u, 4096);
//                    multicore_lockout_end_blocking();
                    restore_interrupts(ints);
                    }

                    for(uint32_t i=0;i<0x50000;i+=4096) {
                    uint32_t ints = save_and_disable_interrupts();
//                    multicore_lockout_start_blocking();     // pause another core
                    flash_range_program(i+0x280000u+(flash_command&0x1f)*0x50000u, (const uint8_t *)(emm+i), 4096);
//                    multicore_lockout_end_blocking();
                    restore_interrupts(ints);
                    }

                    break;  

            }
            flash_command=0;

        }
    }
}

