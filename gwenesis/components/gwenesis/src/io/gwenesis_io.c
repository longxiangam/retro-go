/*
Gwenesis : Genesis & megadrive Emulator.

This program is free software: you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation, either version 3 of the License, or (at your option) any later
version.
This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
You should have received a copy of the GNU General Public License along with
this program. If not, see <http://www.gnu.org/licenses/>.

__author__ = "bzhxx"
__contact__ = "https://github.com/bzhxx"
__license__ = "GPLv3"

*/
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "gwenesis_io.h"
#include "gwenesis_savestate.h"

/*
 * Button mapping (6-button controller support)
 * Low byte (0-7): 3-button compatible
 * High byte (8-15): Extended 6-button buttons
 *
 *  15  14  13  12  11  10   9   8   7   6   5   4   3   2   1   0
 *  -   -   -   - MODE  Z   Y   X Start  A   B   C   R   L   D   U
 */
uint16_t button_state[3] = {0xFFFF, 0xFFFF, 0xFFFF};
static unsigned char gwenesis_io_pad_state[3] = {0x33,0x33,0x33};

#define GWENESIS_IO_VERSION 0x81 /* oversea NTSC model version 81 */
/*
$A10003	:	MODE 	VMOD 	DISK 	RSV 	VER3 	VER2 	VER1 	VER0
MODE (R) 	0: Domestic Model
  	        1: Overseas Model
VMOD (R) 	0: NTSC CPU clock 7.67 MHz
  	        1: PAL CPU clock 7.60 MHz
RSV (R) 	Currently not used
VER3-0 (R)	MEGA DRIVE version is indicated by $0-$F. The present hardware version is indicated by $0. 
$A10003	: DATA 1 ( CTRL1 )
$A10005	: DATA 2 ( CTRL2 )
$A10007	: DATA 3 ( EXP )
$A10009	: CTRL 1
$A1000B	: CTRL 2
$A1000D	: CTRL 3
$A1000F	: TxDATA 1
$A10011	: RxDATA 1
$A10013	: S-CTRL 1
$A10015	: TxDATA 2
$A10017	: RxDATA 2
$A10019	: S-CTRL 2
$A1001B	: TxDATA 3
$A1001D	: RxDATA 3
$A1001F	: S-CTRL 3 

DATA shows the status of each port. The I/O direction of each bit is set by CTRL and S-CTRL.
DATA =	PD7 	PD6 	PD5 	PD4 	PD3 	PD2 	PD1 	PD0

PD7 (RW)
PD6 (RW) TH
PD5 (RW) TR
PD4 (RW) TL
PD3 (RW) RIGHT
PD2 (RW) LEFT
PD1 (RW) DOWN
PD0 (RW) UP 

TH = 0
PD7 0
PD6 TH=0
PD5 Start
PD4 A
PD3 0
PD2 0
PD1 DOWN
PD0 UP    

TH = 1
PD7 0
PD6 TH=1
PD5 B
PD4 C
PD3 RIGHT
PD2 LEFT
PD1 DOWN
PD0 UP   


CTRL designates the I/O direction of each port and the INTERRUPT CONTROL of TH.
CTRL = 	INT 	PC6 	PC5 	PC4 	PC3 	PC2 	PC1 	PC0

INT (RW)	0: TH-INT PROHIBITED
	1: TH-INT ALLOWED
PC6 (RW)	0: PD6 INPUT MODE
	1: OUTPUT MODE
PC5 (RW)	0: PD5 INPUT MODE
	1: OUTPUT MODE
PC4 (RW)	0: PD4 INPUT MODE
	1: OUTPUT MODE
PC3 (RW)	0: PD3 INPUT MODE
	1: OUTPUT MODE
PC2 (RW)	0: PD2 INPUT MODE
	1: OUTPUT MODE
PC1 (RW)	0: PD1 INPUT MODE
	1: OUTPUT MODE
PC0 (RW)	0: PD0 INPUT MODE
	1: OUTPUT MODE  

S-CTRL is for the status, etc. of each port's mode change, baud rate and SERIAL.

S-CTRL = BPS1 	BPS0 	SIN 	SOUT 	RINT 	RERR 	RRDY 	TFUL

SIN (RW) 	0: TR-PARALLEL MODE
	1: SERIAL IN
SOUT (RW)	0: TL-PARALLEL MODE
	1: SERIAL OUT
RINT (RW)	0: Rxd READY INTERRUPT PROHIBITED
	1: Rxd READY INTERRUPT ALLOWED
RERR (R) 	0:
	1: Rxd ERROR
RRDY (R) 	0:
	1: Rxd READY
TFUL (R) 	0:
	1: Txd FULL 
*/

unsigned char io_reg[16] = {GWENESIS_IO_VERSION, /* 0x1 Version */
                             0x7f, 0x7f, 0x7f,   /* 0x3-5-7 JOYPAD DATA 1 2 & EXT */
                             0x00, 0x00, 0x00,   /* 0x9-A-C JOYPAD CTRL 1 2 & EXT */
                             0xff,    0,    0,   /* PORT 1 */
                             0xff,    0,    0,   /* PORT 2 */
                             0xff,    0,    0};  /* PORT 3 */

void gwenesis_io_pad_release_button(int pad, int button)
{
    button_state[pad] |= (1 << button);
}

void gwenesis_io_pad_press_button(int pad, int button)
{
    button_state[pad] &= ~(1 << button);
}

static inline void gwenesis_io_pad_write(int pad, int value)
{
    unsigned char mask = io_reg[pad + 4];

     gwenesis_io_pad_state[pad] &= ~mask;
    gwenesis_io_pad_state[pad] |= value & mask;
 
}

static int th_pulse_count[3] = {0, 0, 0};

void gwenesis_io_new_frame(void)
{
    th_pulse_count[0] = 0;
    th_pulse_count[1] = 0;
    th_pulse_count[2] = 0;
}

static inline unsigned char gwenesis_io_pad_read(int pad)
{
    unsigned char value;
    uint16_t buttons = button_state[pad];
    int pulse = th_pulse_count[pad];

    value = gwenesis_io_pad_state[pad] & 0x40;
    value |= 0x3f;

    if (value & 0x40)  // TH = HIGH
    {
        if (pulse == 3)
        {
            // 6-button 3rd pulse HIGH: D0=Z, D1=Y, D2=X, D3=MODE, D4=B, D5=C
            value &= ~0x0F;
            if (buttons & (1 << 10)) value |= (1 << 0);
            if (buttons & (1 << 9))  value |= (1 << 1);
            if (buttons & (1 << 8))  value |= (1 << 2);
            if (buttons & (1 << 11)) value |= (1 << 3);
            if (buttons & (1 << 5)) value |= (1 << 4);
            if (buttons & (1 << 4)) value |= (1 << 5);
        }
        else
        {
            value &= (buttons & 0x3f);
        }
    }
    else  // TH = LOW
    {
        if (pulse == 3)
        {
            value &= ~0x0F;
            if (buttons & (1 << 7)) value |= (1 << 5);
            if (buttons & (1 << 6)) value |= (1 << 4);
        }
        else
        {
            value &= ((buttons & 3) | ((buttons >> 2) & 0x30));
        }
    }

    return value;
}

void gwenesis_io_write_ctrl(unsigned int address, unsigned int value)
{

    address >>= 1;
 
    // JOYPAD DATA
    if (address >= 0x1 && address <= 0x3)
    {
        int pad = address - 1;
        uint8_t new_th = value & 0x40;
        uint8_t old_th = gwenesis_io_pad_state[pad] & 0x40;

        if (new_th == 0x00 && old_th == 0x40)
        {
            th_pulse_count[pad]++;
            if (th_pulse_count[pad] > 4)
                th_pulse_count[pad] = 0;
        }

        io_reg[address] = value;
        gwenesis_io_pad_write(address - 1, value);
        return;
    }
    // JOYPAD CTRL
    else if (address >= 0x4 && address <= 0x6)
    {
        if (io_reg[address] != value)
        {
            io_reg[address] = value;
            gwenesis_io_pad_write(address - 4, io_reg[address - 3]);
        }
        return;
    }

    return;
}

unsigned int gwenesis_io_read_ctrl(unsigned int address)
{
    address >>= 1;
    if (address >= 0x1 && address < 0x4)
    {
        unsigned char mask = 0x80 | io_reg[address + 3];
        unsigned char value;
        value = io_reg[address] & mask;
        value |= gwenesis_io_pad_read(address - 1) & ~mask;

        return value;
    }
    else
    {
        return io_reg[address];
    }
}

void gwenesis_io_set_reg(unsigned int reg, unsigned int value) {
    io_reg[reg] = value;
    return;
}

void gwenesis_io_save_state() {
    SaveState* state;
    state = saveGwenesisStateOpenForWrite("io");
    saveGwenesisStateSetBuffer(state, "button_state", button_state, sizeof(button_state));
    saveGwenesisStateSetBuffer(state, "gwenesis_io_pad_state", gwenesis_io_pad_state, sizeof(gwenesis_io_pad_state));
    saveGwenesisStateSetBuffer(state, "io_reg", io_reg, sizeof(io_reg));
}

void gwenesis_io_load_state() {
    SaveState* state = saveGwenesisStateOpenForRead("io");
    saveGwenesisStateGetBuffer(state, "button_state", button_state, sizeof(button_state));
    saveGwenesisStateGetBuffer(state, "gwenesis_io_pad_state", gwenesis_io_pad_state, sizeof(gwenesis_io_pad_state));
    saveGwenesisStateGetBuffer(state, "io_reg", io_reg, sizeof(io_reg));
}
