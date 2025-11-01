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
//  (GP25: RESET -> Interrupt)
//  (GP28: MERQ)
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
//#include "hardware/pio.h"
#include "hardware/clocks.h"
#include "hardware/timer.h"
#include "hardware/uart.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "hardware/vreg.h"

#include "misc.h"

#define USE_EMM

// ROM configuration
// WeACT RP2350B with 16MiB Flash
// Kanji ROM        128KiB @ 0x10050000
// Jisho ROM        256KiB @ 0x10060000
// Data for 1R12    64KiB * 64 = 4MiB @ 0x10080000
// LittleFS       11MiB (Not Used)

#define ROMBASE 0x10080000u

uint8_t *kanjirom=(uint8_t *)(ROMBASE-0x60000);
uint8_t *jishorom=(uint8_t *)(ROMBASE-0x40000);
uint8_t *romslots=(uint8_t *)(ROMBASE);

// Define the flash sizes
// This is setup to read a block of the flash from the end 
#define BLOCK_SIZE_BYTES (FLASH_SECTOR_SIZE)
// for 16M flash
#define HW_FLASH_STORAGE_BYTES  (8192 * 1024)
#define HW_FLASH_STORAGE_BASE   (1024*1024*16 - HW_FLASH_STORAGE_BYTES) 

// RAM configuration
// MZ-1R18: 64KiB
// MZ-1R12: 64KiB
// EMM:    320KiB

uint8_t mz1r12[0x10000];
uint8_t mz1r18[0x10000];
uint8_t emm[0x50000];

#define MAXROMPAGE 64       // = 128KiB * 64 pages = 8MiB

volatile uint8_t rompage=0;  // Page No of MZ-1R12 (64KiB/page)

volatile uint32_t kanjiptr=0;
volatile uint32_t dictptr=0;
volatile uint32_t kanjictl=0;
volatile uint32_t mz1r18ptr=0;
volatile uint32_t mz1r12ptr=0;
volatile uint32_t emmptr=0;
volatile uint32_t flash_command=0;

// uint8_t __attribute__  ((aligned(sizeof(unsigned char *)*4096))) flash_buffer[4096];

//
//  reset

//void __not_in_flash_func(z80reset)(void) {
void __not_in_flash_func(z80reset)(uint gpio,uint32_t event) {

    gpio_acknowledge_irq(25,GPIO_IRQ_EDGE_FALL);

    kanjiptr=0;
    dictptr=0;
    kanjictl=0;
    mz1r18ptr=0;
    mz1r12ptr=0;
    emmptr=0;

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
            return;

        case 0xf9:  // MZ-1R12 ptr low
            mz1r12ptr=data+(mz1r12ptr&0xff00);
            return;

        case 0xfa:
            mz1r12[mz1r12ptr&0xffff]=data;
            mz1r12ptr++;
            return;

        //  MZ-1R12 control 
#if 1
        case 0xfb:
            flash_command=0x10000000+(data&0x3f);
            return;
#endif

        // EMM
        case 0:
            emmptr=(emmptr&0xfff00)+(data);
            return;

        case 1:
            emmptr=(emmptr&0xf00ff)+(data<<8);
            return;           

        case 2:
            emmptr=(emmptr&0xffff)+(data<<16);
            return; 

        case 3:
            emm[emmptr&0x4ffff]=data;
            emmptr++;
            return;

    }
 
    return;

}

void init_emulator(void) {

    // Erase Gamepad info

    kanjiptr=0;
    mz1r12ptr=0;
    mz1r18ptr=0;

    // Load DUMMY data to MZ-1R12

//    memcpy(mz1r12,romslots,0x10000);
    memset(mz1r12,0xaa,0x10000);

}

// Main thread (Core1)

void __not_in_flash_func(main_core1)(void) {

    volatile uint32_t bus;

    uint32_t control,address,data,response;
    uint32_t needwait=0;

//    multicore_lockout_victim_init();

    gpio_init_mask(0xffffffff);
    gpio_set_dir_all_bits(0x00000000);  // All pins are INPUT

// DEBUG
    gpio_set_dir(25,1);
    gpio_put(25,0);

    while(1) {

        bus=gpio_get_all();

        control=bus&0xe0000000;

        // Check IO Read

        if(control==0x40000000) {

            address=(bus&0xffff00)>>8;

            switch(address&0xff) {

                // MZ-1R23 & 1R24

                case 0xb9:
                    // ENABLE WAIT
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
                        data=jishorom[(kanjictl&3)<<16+(dictptr++)];
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
                    data=mz1r12[mz1r12ptr&0xffff];
                    mz1r12ptr++;
                    response=1;
                    break;

                // EMM
#ifdef USE_EMM
                case 0x3:
                    data=emm[emmptr&0x4ffff];
                    emmptr++;
                    response=1;
                    break;
#endif

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

        if(control==0x80000000) {

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

    multicore_launch_core1(main_core1);

    init_emulator();

    // Set RESET# interrupt

//    gpio_set_irq_enabled_with_callback(25,GPIO_IRQ_EDGE_FALL,true,z80reset);

    while(1) {

        if(flash_command!=0) {

            switch(flash_command&0xf0000000) {

                case 0x10000000:
                    memcpy(mz1r12,romslots+(flash_command&0x3f)*0x10000,0x10000);
                    break;
            }
            flash_command=0;

        }
    }
}

