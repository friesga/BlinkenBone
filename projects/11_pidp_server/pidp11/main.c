/* main.c: Blinkenlight API server to run on "PiDP11" replica

 Copyright (c) 2015-2016, Joerg Hoppe
 j_hoppe@t-online.de, www.retrocmp.com

 Permission is hereby granted, free of charge, to any person obtaining a
 copy of this software and associated documentation files (the "Software"),
 to deal in the Software without restriction, including without limitation
 the rights to use, copy, modify, merge, publish, distribute, sublicense,
 and/or sell copies of the Software, and to permit persons to whom the
 Software is furnished to do so, subject to the following conditions:

 The above copyright notice and this permission notice shall be included in
 all copies or substantial portions of the Software.

 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 JOERG HOPPE BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

 01-Apr-2016  OV    almost perfect before VCF SE
 22-Mar-2016  JH    allow a control value to be distributed over several hw registers
 20-Mar-2016  OV    test hack to convert pidp8 into pidp11 server.

 15-Mar-2016  JH	V 1.3 Low-pass for SimH output, display patterns for brightness levels
 09-Mar-2016  JH	V 1.2 inverted "Deposit" switch
 06-Mar-2016  JH    renamed from "blinkenlightd" to "pidp8_blinkenlightd"
 22-Feb-2016  JH	V 1.1 added panel modes LAMPTEST, POWERLESS
 13-Nov-2015  JH    V 1.0 created


 Blinkenlight API server, which controls lamps and switches
 on the Raspberry based "PiPDP11"

 Like the generic blinkenlightd,
 - PiDP11 controls are fix wired in, no config file
 - Hardware interface to Raspberry is "gpio11.c"

 */

#define MAIN_C_

#define VERSION	"v1.3.0"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <errno.h>
#include <pthread.h>

#include "blinkenlight_panels.h"
#include "blinkenlight_api_server_procs.h"

///// for Blinkenlight API server
#include "rpc_blinkenlight_api.h"
#include <rpc/pmap_clnt.h> // different name under "oncrpc for windows"?
#ifndef SIG_PF
#define SIG_PF void(*)(int)
#endif

#include "bitcalc.h"
#include "print.h"

#include "main.h"
#include "gpio.h"
#include "gpiopattern.h"

char program_info[1024];
char program_name[1024]; // argv[0]
char program_options[1024]; // argv[1.argc-1]
int opt_test = 0;
int opt_background = 0;

int knobValue[2] = {7,0};	// default start value for knobs. 0=ADDR, 1=DATA


/*
 *	PiDP11 controls wich are accessible over Blinkenlight API
 */
blinkenlight_control_t *control_raw_switchstatus[2];
blinkenlight_control_t *control_raw_ledstatus[6]; //6 not 8 for PiDP11

// input controls on the panel
// ------------- from realcons_console_pdp11_70.h ----------------------
blinkenlight_control_t *switch_SR, *switch_LOADADRS, *switch_EXAM, *switch_DEPOSIT, *switch_CONT,
        *switch_HALT, *switch_S_BUS_CYCLE, *switch_START, *switch_DATA_SELECT, *switch_ADDR_SELECT,
        *switch_LAMPTEST, *switch_PANEL_LOCK, *switch_POWER;
//	*switch_DATA_SELECT_R, *switch_ADDR_SELECT_R;	// real rotary encoder

// output controls on the panel
blinkenlight_control_t *leds_ADDRESS, *leds_DATA, *led_PARITY_HIGH, *led_PARITY_LOW, *led_PAR_ERR,
        *led_ADRS_ERR, *led_RUN, *led_PAUSE, *led_MASTER, *leds_MMR0_MODE, *led_DATA_SPACE,
        *led_ADDRESSING_16, *led_ADDRESSING_18, *led_ADDRESSING_22, 
	*leds_DATA_SELECT, *leds_ADDR_SELECT;

// bits in artificially constructed control value for DATA SELECT KNOB feedback LEDs



#define VALMASK_LED_BUS_REG 0x01
#define VALMASK_LED_DATA_PATHS 0x02
#define VALMASK_LED_UADDRS 0x04
#define VALMASK_LED_DISP_REG 0x08



// bits in artificially constructed control value for ADDR SELECT KNOB feedback LEDs
/*    #define VALMASK_LED_USER_D 0x01
    #define VALMASK_LED_SUPER_D 0x02
    #define VALMASK_LED_KERNEL_D 0x04
    #define VALMASK_LED_CONS_PHY 0x08
#define VALMASK_LED_USER_I 0x10
#define VALMASK_LED_SUPER_I 0x20
#define VALMASK_LED_KERNEL_I 0x40
#define VALMASK_LED_PROG_PHY 0x80
*/
#define VALMASK_LED_PROG_PHY 0x01
#define VALMASK_LED_CONS_PHY 0x02
#define VALMASK_LED_KERNEL_D 0x04
#define VALMASK_LED_SUPER_D 0x08
#define VALMASK_LED_USER_D 0x10
#define VALMASK_LED_USER_I 0x20
#define VALMASK_LED_SUPER_I 0x40
#define VALMASK_LED_KERNEL_I 0x80



// ---------------------------------------------------------------------

/*
 *	RPC server callbacks:
 *	here conversion between PiDP11 gpio and Blinkenlight API is done!
 */
static void on_blinkenlight_api_panel_get_controlvalues(blinkenlight_panel_t *p)
{
    // gets called when RPC client wants panel input control values
    //- converts gpio switches to Blinkenlight API switch conrols (RPI->Blinkenlight API)
    unsigned i;

    for (i = 0; i < p->controls_count; i++) {
        blinkenlight_control_t *c = &p->controls[i];
        if (c->is_input) {
         
            if (c == switch_POWER)
             	c->value = 1; // send "power"" switch as ON

            else if (c == switch_PANEL_LOCK)
             	c->value = 0; // send "panel lock" switch as OFF

            else 
	    {
                // mount switch value from register bit fields
                unsigned i_register_wiring;
                blinkenlight_control_blinkenbus_register_wiring_t *bbrw;
                c->value = 0;
                for (i_register_wiring = 0; i_register_wiring < c->blinkenbus_register_wiring_count;
                        i_register_wiring++) {
                    uint32_t regvalbits; // value of current register
                    // for all registers assigned whole or in part to control
                    bbrw = &(c->blinkenbus_register_wiring[i_register_wiring]);

                    regvalbits = gpio_switchstatus[bbrw->blinkenbus_register_address];
                    if (bbrw->blinkenbus_levels_active_low) //  inputs "low active"
                        regvalbits = ~regvalbits;
                    regvalbits &= bbrw->blinkenbus_bitmask; // bits of value, unshiftet
                    regvalbits >>= bbrw->blinkenbus_lsb;
                    // OR in the bits of current register
                    c->value |= (uint64_t) regvalbits << bbrw->control_value_bit_offset;
                }
                if (c->mirrored_bit_order)
                    c->value = mirror_bits(c->value, c->value_bitlen);
                // individual fixup/logic

                if (c == switch_ADDR_SELECT) {
		    c->value = 7-knobValue[0];
                    switch (c->value) {

                    // INSERT CORRECT HW VALUES IN CASE SELECTORS
                   case 0:
                        leds_ADDR_SELECT->value = VALMASK_LED_PROG_PHY ; // feedback
                        break;
                   case 1:
                        leds_ADDR_SELECT->value = VALMASK_LED_CONS_PHY ; // feedback
                        break;
                   case 2:
                        leds_ADDR_SELECT->value = VALMASK_LED_KERNEL_D ; // feedback
                        break;
                   case 3:
                        leds_ADDR_SELECT->value = VALMASK_LED_SUPER_D ; // feedback
                        break;
                   case 4:
                        leds_ADDR_SELECT->value = VALMASK_LED_USER_D ; // feedback
                        break;
                   case 5:
                        leds_ADDR_SELECT->value = VALMASK_LED_USER_I ; // feedback
                        break;
                   case 6:
                        leds_ADDR_SELECT->value = VALMASK_LED_SUPER_I ; // feedback
                        break;
                   case 7:
                        leds_ADDR_SELECT->value = VALMASK_LED_KERNEL_I ; // feedback
                        break;



                    };

                } else if (c == switch_DATA_SELECT) {
		    c->value = 3- knobValue[1];
                    switch (c->value) {
                    // INSERT CORRECT HW VALUES IN CASE SELECTORS
                    case 0:
                        leds_DATA_SELECT->value = VALMASK_LED_BUS_REG ; // feedback
                        break;
                    case 1:
                        leds_DATA_SELECT->value = VALMASK_LED_DATA_PATHS ; // feedback
                        break;
                   case 2:
                        leds_DATA_SELECT->value = VALMASK_LED_UADDRS ;
                        break;
                    case 3:
                        leds_DATA_SELECT->value = VALMASK_LED_DISP_REG ;
                        break;

                    };
                }
            }
        }
    }
}

static void on_blinkenlight_api_panel_set_controlvalues(blinkenlight_panel_t *p, int force_all)
{
    // gets called when RPC client updated panel control values
    // converts Blinkenlight API leds to gpio (Blinkenligt API -> RPI)

    // the averaging thread needs to be informed about the panel
    // THIS WORKS ONLY BECAUSE ONLY ONE PANEL is provided by this server!
    // NO PANEL SWITCH ALLOWED!

// ov workaround 1 - do not let simh mess with knob leds
//int tmpADDR = leds_ADDR_SELECT->value;
//int tmpDATA = leds_DATA_SELECT->value;

    gpiopattern_blinkenlight_panel = p;
    // this also start the thread on first transmission, if gpiopattern_blinkenlight_panel gets != NULL

// ov workaround 2 - do not let simh mess with knob leds
//leds_ADDR_SELECT->value = 0; //tmpADDR;
//leds_DATA_SELECT->value = 0; //tmpDATA;


    // gpiopattern_update_leds() ; // just forward to pattern generator
}

static int on_blinkenlight_api_panel_get_state(blinkenlight_panel_t *p)
{
    // dummy: "all BlinkenBoards of panel active"
    return RPC_PARAM_VALUE_PANEL_BLINKENBOARDS_STATE_ACTIVE;
}

static void on_blinkenlight_api_panel_set_state(blinkenlight_panel_t *p, int new_state)
{
    // noop
}

// set get selftest/powerless mode
static int on_blinkenlight_api_panel_get_mode(blinkenlight_panel_t *p)
{
    return p->mode;
}

static void on_blinkenlight_api_panel_set_mode(blinkenlight_panel_t *p, int new_state)
{
    p->mode = new_state;
    // GPIO logic here
    on_blinkenlight_api_panel_set_controlvalues(p, 1);
}

static char *on_blinkenlight_api_get_info()
{
    static char buffer[1024];

    sprintf(buffer, "Server info ...............: %s\n"
            "Server program name........: %s\n"
            "Server command line options: %s\n"
            "Server compile time .......: " __DATE__ " " __TIME__ "\n", //
            program_info, program_name, program_options);

    return buffer;
}

/*
 * Start the parallel thread which operates the GPIO mux.
 */
void *blink(void *ptr); // the real-time GPIO multiplexing process to start up
void *gpiopattern_update_leds(void *ptr); // the averaging thread

pthread_t blink_thread;
int blink_thread_terminate = 0;
pthread_t gpiopattern_thread;
int gpiopattern_thread_terminate = 0;

static void gpio_mux_thread_start()
{
    int res;
//	printf("\nPiDP FP driver 3\n");
    res = pthread_create(&blink_thread, NULL, blink, &blink_thread_terminate);
    if (res) {
        fprintf(stderr, "Error creating gpio_mux thread, return code %d\n", res);
        exit(EXIT_FAILURE);
    }
    printf("Created \"gpio_mux\" thread\n");

    sleep(2); // allow 2 sec for multiplex to start
}

static void gpiopattern_start_thread()
{
    int res;
    gpiopattern_blinkenlight_panel = NULL; // wait for first API transmission to start

    res = pthread_create(&gpiopattern_thread, NULL, gpiopattern_update_leds,
            &gpiopattern_thread_terminate);
    if (res) {
        fprintf(stderr, "Error creating gpiopattern thread, return code %d\n", res);
        exit(EXIT_FAILURE);
    }
    printf("Created \"gpiopattern_update_leds\" thread\n");
}

/******************************************************
 * Server for Blinkenlight API
 * see code generated by rpcgen
 * DOES NEVER END!
 */
void blinkenlight_api_server(void)
{
    register SVCXPRT *transp;

// entry to server stub

    void blinkenlightd_1(struct svc_req *rqstp, register SVCXPRT *transp);

    pmap_unset(BLINKENLIGHTD, BLINKENLIGHTD_VERS);

    transp = svcudp_create(RPC_ANYSOCK);
    if (transp == NULL) {
        print(LOG_ERR, "%s", "cannot create udp service.");
        exit(1);
    }
    if (!svc_register(transp, BLINKENLIGHTD, BLINKENLIGHTD_VERS, blinkenlightd_1, IPPROTO_UDP)) {
        print(LOG_ERR, "%s", "unable to register (BLINKENLIGHTD, BLINKENLIGHTD_VERS, udp).");
        exit(1);
    }

    transp = svctcp_create(RPC_ANYSOCK, 0, 0);
    if (transp == NULL) {
        print(LOG_ERR, "%s", "cannot create tcp service.");
        exit(1);
    }
    if (!svc_register(transp, BLINKENLIGHTD, BLINKENLIGHTD_VERS, blinkenlightd_1, IPPROTO_TCP)) {
        print(LOG_ERR, "%s", "unable to register (BLINKENLIGHTD, BLINKENLIGHTD_VERS, tcp).");
        exit(1);
    }

    // svc_run();
    // alternate implementation of svn_run() with periodically timeout and
    // 	calling of callback
    {
        fd_set readfds;
        struct timeval tv;
        int dtbsz = getdtablesize();
        for (;;) {
            readfds = svc_fdset;
            tv.tv_sec = 0;
            tv.tv_usec = 1000 * 2; // every 10 ms*time_slice_ms;
            switch (select(dtbsz, &readfds, NULL, NULL, &tv)) {
            case -1:
                if (errno == EINTR)
                    continue;
                perror("select");
                return;
            case 0: // timeout
                    // provide the panel simulation with computing time
                // not needed:RPC calls control value get/set callbacks
                break;
            default:
                svc_getreqset(&readfds);
                break;
            }
            /**/
        }
    }

    print(LOG_ERR, "%s", "svc_run returned");
    exit(1);
    /* NOTREACHED */

}

/*
 * print program info
 */
void info(void)
{
    print(LOG_INFO, "\n");
    print(LOG_NOTICE, "*** pidp11_blinkenlightd %s - server for PiDP11 ***\n", VERSION);
    print(LOG_NOTICE, "    Compiled " __DATE__ " " __TIME__ "\n");
    print(LOG_NOTICE, "    Copyright (C) 2015-2016 Joerg Hoppe, Oscar Vermeulen.\n");
    print(LOG_NOTICE, "    www.retrocmp.com, obsolescence.wix.com/obsolescence\n");
    print(LOG_NOTICE, "\n");
}

/*
 * print help
 */
static void help(void)
{
    fprintf(stderr, "\n");
    fprintf(stderr, "pidp11_blinkenlightd %s - Blinkenlight RPC server for PiDP11 \n",
    VERSION);
    fprintf(stderr, "  (compiled " __DATE__ " " __TIME__ ")\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "Call:\n");
    fprintf(stderr, "pidp11_blinkenlightd [-b] [-v] [-t]\n");
    fprintf(stderr, "\n");
//	fprintf(stderr, "- <port>:               TCP port for RCP access.\n") ;
    fprintf(stderr, "-b               : background operation: print to syslog (view with dmesg)\n");
    fprintf(stderr, "                  default output is stderr\n");
    fprintf(stderr, "-v               : verbose: tell what I'm doing\n");
    fprintf(stderr, "-t               : Test mode\n");
    fprintf(stderr, "\n");
}

/*
 * read commandline paramaters into global vars
 * result: 1 = OK, 0 = error
 */
static int parse_commandline(int argc, char **argv)
{
    int i;
    int c;

    strcpy(program_name, argv[0]);
    strcpy(program_options, "");
    for (i = 1; i < argc; i++) {
        if (i > 1)
            strcat(program_options, " ");
        strcat(program_options, argv[i]);
    }

    opterr = 0;

    while ((c = getopt(argc, argv, "bvt")) != -1)
        switch (c) {
        case 'v':
            print_level = LOG_DEBUG;
            break;
        case 'b':
            opt_background = 1;
            break;
        case 't':
            opt_test = 1;
            break;
        case '?': // getopt detected an error. "opterr=0", so own error message here
            if (isprint(optopt))
                fprintf(stderr, "Unknown option `-%c'.\n", optopt);
            else
                fprintf(stderr, "Unknown option character `\\x%x'.\n", optopt);
            return 0;
            break;
        default:
            abort(); // getopt() got crazy?
            break;
        }

    // start logging
    print_open(opt_background); // if background, then syslog

    return 1;
}

/*
 * Define part of the fix controls of the PiDP11 == PDP11/70
 * for the Blinkenlight API interface structs
 * - control_value_bit_offset: position of bitfield in final control value
 * - bitlen: count of bits in status register to be mounted into value
 * - gpio_switchstatus_index: gpio_switchstatus[idx] is mounted here
 * - bit_offset: LSB of bitfield in gpio_switchstatus[] to be shifted this much
 *
 * See http://retrocmp.com/projects/blinkenbone/blinkenbone-physical-panels/173-blinkenbone-blinkenlightd-patch-field-decoding-and-console-panel-simulator
 */
static blinkenlight_control_t *define_switch_slice(blinkenlight_panel_t *p, char *name,
        unsigned control_value_bit_offset, unsigned bitlen, unsigned gpio_switchstatus_index,
        unsigned bit_offset)
{
    blinkenlight_control_t *c;
    blinkenlight_control_blinkenbus_register_wiring_t *bbrw;

    // control already there?
    c = blinkenlight_panels_get_control_by_name(blinkenlight_panel_list, p, name, /*is_input*/1);
    if (c == NULL) {
        c = blinkenlight_add_control(blinkenlight_panel_list, p);
        strcpy(c->name, name);
        c->is_input = 1;
        c->type = input_switch;
        c->encoding = binary;
        c->radix = 8; // display octal
    }
    // shift and mask data are saved in the "register wiring" struct.
    bbrw = blinkenlight_add_register_wiring(c);
    bbrw->blinkenbus_board_address = 0; // simulate 1 board with unlimited registers
    bbrw->board_register_address = gpio_switchstatus_index;
    bbrw->control_value_bit_offset = control_value_bit_offset;
    bbrw->blinkenbus_lsb = bit_offset;
    bbrw->blinkenbus_msb = bbrw->blinkenbus_lsb + bitlen - 1;
    // all switches invers: bit 1 => switch up in "0" position
    bbrw->blinkenbus_levels_active_low = 1;

    return c;
}

// define outputs, see register_switch_slice()
static blinkenlight_control_t *define_led_slice(blinkenlight_panel_t *p, char *name,
        unsigned control_value_bit_offset, unsigned bitlen, unsigned gpio_ledstatus_index,
        unsigned bit_offset)
{
    blinkenlight_control_t *c;
    blinkenlight_control_blinkenbus_register_wiring_t *bbrw;

    c = blinkenlight_panels_get_control_by_name(blinkenlight_panel_list, p, name, /*is_input*/0);
    if (c == NULL) {
        c = blinkenlight_add_control(blinkenlight_panel_list, p);
        strcpy(c->name, name);
        c->is_input = 0;
        c->type = output_lamp;
        c->encoding = binary;
//        c->fmax = 4; // lamps are slow light bulbs, low-pass with 4 Hz
        c->fmax = 10; // Joerg's setting for pdp11 java panel
        c->radix = 8; // display octal
    }
    bbrw = blinkenlight_add_register_wiring(c);
    bbrw->blinkenbus_board_address = 0; // simulate 1 board with unlimited registers
    bbrw->board_register_address = gpio_ledstatus_index;
    bbrw->control_value_bit_offset = control_value_bit_offset;
    bbrw->blinkenbus_lsb = bit_offset;
    bbrw->blinkenbus_msb = bbrw->blinkenbus_lsb + bitlen - 1;
    bbrw->blinkenbus_levels_active_low = 0; // LED drivers not inverting

    return c;
}

static void register_controls()
{
    blinkenlight_panel_t *p;

    // one global panel list ...
    blinkenlight_panel_list = blinkenlight_panels_constructor();
    // ... with one panel
    p = blinkenlight_add_panel(blinkenlight_panel_list);
    strcpy(p->name, "11/70");
    strcpy(p->info, "PiDP11 system");

    /*
     * Construct high-level Blinkenlight API controls from
     * hardware switch- and led-registers
     */

    //SR consists of 12 and 10 bit subparts
    switch_SR = define_switch_slice(p, "SR", 0, 12, 0, 0); // 0, 0xfff. bits 0..11
    switch_SR = define_switch_slice(p, "SR", 12, 10, 1, 0); // 1  0x3ff, bits 12..21

    // LAMP TEST: unlike on DEC panel readable over API, but locally wired function
    // see gpiopattern.value2gpio_ledstatus_value()
    switch_LAMPTEST    = define_switch_slice(p, "LAMPTEST", 0, 1, 2, 0); // 2, 0x01 (or should it be 1,0,0 fur null?)

    switch_LOADADRS = define_switch_slice(p, "LOAD ADRS", 0, 1, 2, 1); // 2, 0x02
    switch_EXAM = define_switch_slice(p, "EXAM", 0, 1, 2, 2); // 2, 0x04
    switch_DEPOSIT = define_switch_slice(p, "DEPOSIT", 0, 1, 2, 3); // 2, 0x08
    switch_CONT = define_switch_slice(p, "CONT", 0, 1, 2, 4); // 2, 0x010
    switch_HALT = define_switch_slice(p, "HALT", 0, 1, 2, 5); // 2, 0x020
    switch_S_BUS_CYCLE = define_switch_slice(p, "S_BUS_CYCLE", 0, 1, 2, 6); // 2, 0x040
    switch_START = define_switch_slice(p, "START", 0, 1, 2, 7); // 2, 0x080

    // Constant values set in on_blinkenlight_api_panel_get_controlvalues()

    leds_ADDRESS = define_led_slice(p, "ADDRESS", 0, 12, 0, 0); // 0,0xfff, bits 0..11
//    leds_ADDRESS = define_led_slice(p, "ADDRESS", 12, 10, 0, 0); // 0,0xfff, bits 12..21
// oscar correction
    leds_ADDRESS = define_led_slice(p, "ADDRESS", 12, 10, 1, 0); // 0,0xfff, bits 12..21
// end of oscar correction

    leds_DATA = define_led_slice(p, "DATA", 0, 12, 3, 0); // 3,0xfff, bits 0..11
//    leds_DATA = define_led_slice(p, "DATA", 12, 4, 3, 0); // 4,0xf, bits 12..15
// oscar correction
    leds_DATA = define_led_slice(p, "DATA", 12, 4, 4, 0); // 4,0xf, bits 12..15
// end of oscar correction

    led_PARITY_HIGH = define_led_slice(p, "PARITY_HIGH", 0, 1, 4, 5); // 4, 0x20
    led_PARITY_LOW = define_led_slice(p, "PARITY_LOW", 0, 1, 4, 4); // 4, 0x10
    led_PAR_ERR = define_led_slice(p, "PAR_ERR", 0, 1, 2, 11); // 2, 0x800
    led_ADRS_ERR = define_led_slice(p, "ADRS_ERR", 0, 1, 2, 10); // 2, 0x400
    led_RUN = define_led_slice(p, "RUN", 0, 1, 2, 9); // 2, 0x200
    led_PAUSE = define_led_slice(p, "PAUSE", 0, 1, 2, 8); // 2, 0x100
    led_MASTER = define_led_slice(p, "MASTER", 0, 1, 2, 7); // 2, 0x80
    // Problem: MMR0_MODE needs translation to leds.
    // 0 = Kernel, 1= off,  2 = Super, 3 = User
    // the wiring here is ignored; see handcoding in gpiopattern.value2gpio_ledstatus_value()

//MMRfix
    leds_MMR0_MODE = define_led_slice(p, "MMR0_MODE", 0, 3, 2, 4);

    led_DATA_SPACE = define_led_slice(p, "DATA_SPACE", 0, 1, 2, 3); // 2, 0x08
    led_ADDRESSING_16 = define_led_slice(p, "ADDRESSING_16", 0, 1, 2, 2); // 2, 0x04
    led_ADDRESSING_18 = define_led_slice(p, "ADDRESSING_18", 0, 1, 2, 1); // 2, 0x02
    led_ADDRESSING_22 = define_led_slice(p, "ADDRESSING_22", 0, 1, 2, 0); // 2, 0x01

    // knobs: raw data like 3/2 bit switch. encoding see
    // on_blinkenlight_api_panel_get_controlvalues()
//jorg's patch 20160323: switch slice not led slice

// this is for real encoder addressing (0 1 2 3 0 sequences visible)
//    switch_ADDR_SELECT = define_switch_slice(p, "ADDR_SELECT", 0, 3, 2, 8); // conflict 2 or 3 bits xxxxxxxxxxxxx
//    switch_DATA_SELECT = define_switch_slice(p, "DATA_SELECT", 0, 2, 2, 10);
// this is to fake knob value on SR panel
    switch_ADDR_SELECT = define_switch_slice(p, "ADDR_SELECT", 0, 3, 0, 0); 
    switch_DATA_SELECT = define_switch_slice(p, "DATA_SELECT", 0, 2, 0, 3);

    // ADDR_SELECT & DATA_SELECT feedback LEDs:
    // unlike on DEC panel they are visible over API, but always locally override with
    // knob positons. See on_blinkenlight_api_panel_get_controlvalues()


    // bit encoding in API value: BUG? BELOW ORDER INCORRECT ACC TO JAVA CODE
    // 0..7 = user_d, super_d, kernel_d, cons_phy, user_i, super_i, kernel_i, prog_phy
    // see VALMASK_LED_*
    leds_ADDR_SELECT = define_led_slice(p, "ADDR_SELECT_FEEDBACK", 0, 4, 4, 6);
    leds_ADDR_SELECT = define_led_slice(p, "ADDR_SELECT_FEEDBACK", 4, 4, 5, 5);


    // bit encoding in API value: BUG? BELOW ORDER INCORRECT ACC TO JAVA CODE
    //0..3 = data_paths, bus_reg, muaddr, disp_reg
    // see VALMASK_LED_*
    leds_DATA_SELECT = define_led_slice(p, "DATA_SELECT_FEEDBACK", 0, 2, 4, 10); // bus_reg, data_paths
    leds_DATA_SELECT = define_led_slice(p, "DATA_SELECT_FEEDBACK", 2, 2, 5, 10); // disp_reg, muaddr






                        // TODO: what signals come out? Extend SimH 11/70 with POWER signal, like PDP8I ?
    switch_PANEL_LOCK = define_switch_slice(p, "PANEL_LOCK", 0, 1, 0, 0); // dummy, always 0
    switch_POWER = define_switch_slice(p, "POWER", 0, 1, 0, 0); // dummy, ignored, always 0

    blinkenlight_panels_config_fixup(blinkenlight_panel_list);
}

/*
 *
 */
int main(int argc, char *argv[])
{

    print_level = LOG_NOTICE;
    // print_level = LOG_DEBUG;
    if (!parse_commandline(argc, argv)) {
        help();
        return 1;
    }
    sprintf(program_info, "pidp11_blinkenlightd - Blinkenlight API server daemon for PiDP11 %s",
    VERSION);

    info();

    print(LOG_INFO, "Start\n");

    // link set/get events
    blinkenlight_api_panel_get_controlvalues_evt = on_blinkenlight_api_panel_get_controlvalues;
    blinkenlight_api_panel_set_controlvalues_evt = on_blinkenlight_api_panel_set_controlvalues;
    blinkenlight_api_panel_get_state_evt = on_blinkenlight_api_panel_get_state;
    blinkenlight_api_panel_set_state_evt = on_blinkenlight_api_panel_set_state;
    blinkenlight_api_panel_get_mode_evt = on_blinkenlight_api_panel_get_mode;
    blinkenlight_api_panel_set_mode_evt = on_blinkenlight_api_panel_set_mode;
    blinkenlight_api_get_info_evt = on_blinkenlight_api_get_info;

    register_controls();

    if (opt_test) {
        printf("Dump of register <-> control data struct:\n");
        blinkenlight_panels_diagprint(blinkenlight_panel_list, stdout);
        exit(0);
    }

    gpio_mux_thread_start();
    gpiopattern_start_thread();

    blinkenlight_api_server();
    // does never end!

    print_close(); // well ....

    blinkenlight_panels_destructor(blinkenlight_panel_list);

    return 0;
}

