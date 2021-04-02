/*
#######################################################################
#                                                                     #
# prawnblaster.c                                                      #
#                                                                     #
# Copyright 2021, Philip Starkey                                      #
#                                                                     #
# Serial communication code based on the PineBlaster                  #
#   https://github.com/labscript-suite/pineblaster                    #
#   Copyright 2013, Christopher Billington                            #
#                                                                     #
# This file is used to flash a Raspberry Pi Pico microcontroller      #
# prototyping board to create a PrawnBlaster (see readme.txt and      #
# http://hardware.labscriptsuite.org).                                #
# This file is licensed under the Simplified BSD License.             #
# See the license.txt file for the full license.                      #
#                                                                     #
#######################################################################
*/

#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/dma.h"
#include "hardware/pio.h"
#include "hardware/pll.h"
#include "hardware/clocks.h"
#include "hardware/structs/pll.h"
#include "hardware/structs/clocks.h"

#include "pseudoclock.pio.h"

int DEBUG;

// Can't seem to have this be used to define array size even though it's a constant
const unsigned int max_instructions = 30000;
const unsigned int max_waits = 400;
// max_instructions*2 + 8
unsigned int instructions[60008];
// max_waits + 4
unsigned int waits[404];

char readstring[256] = "";
// This contains the number of clock cycles for a half period, which is currently 5 (there are 5 ASM instructions)
const unsigned int non_loop_path_length = 5;

uint OUT_PINS[4];
uint IN_PINS[4];

int num_pseudoclocks_in_use;

// Mutex for status
static mutex_t status_mutex;

// STATUS flag
int status;
#define STOPPED 0
#define TRANSITION_TO_RUNNING 1
#define RUNNING 2
#define ABORT_REQUESTED 3
#define ABORTING 4
#define ABORTED 5
#define TRANSITION_TO_STOP 6

// Clock status flag
int clock_status;
#define INTERNAL 0
#define EXTERNAL 1

struct pseudoclock_config
{
    PIO pio;
    uint sm;
    uint OUT_PIN;
    uint IN_PIN;
    int instructions_dma_channel;
    int waits_dma_channel;
    int words_to_send;
    int waits_to_send;
    bool configured;
};


// Thread safe functions for getting/setting status
int get_status()
{
    mutex_enter_blocking(&status_mutex);
    int status_copy = status;
    mutex_exit(&status_mutex);
    return status_copy;
}

void set_status(int new_status)
{
    mutex_enter_blocking(&status_mutex);
    status = new_status;
    mutex_exit(&status_mutex);
}

bool configure_pseudoclock_pio_sm(pseudoclock_config *config, uint prog_offset, uint32_t hwstart, int max_instructions_per_pseudoclock, int max_waits_per_pseudoclock)
{
    // Claim the PUI
    pio_claim_sm_mask(config->pio, 1u << config->sm);

    // Zero out waits array
    int max_waits = (max_waits_per_pseudoclock + 1);
    for (int i = config->sm * max_waits; i < (config->sm + 1) * max_waits; i++)
    {
        waits[i] = 0;
    }

    // Find the number of 32 bit words to send
    int words_to_send = 0;
    int wait_count = 1; // We always send a stop message
    bool previous_instruction_was_wait = false;
    int max_words = (max_instructions_per_pseudoclock * 2 + 2);
    for (int i = config->sm * max_words; i < ((config->sm + 1) * max_words); i += 2)
    {
        if (instructions[i] == 0 && instructions[i + 1] == 0)
        {
            words_to_send = i + 2 - config->sm * max_words;
            break;
        }
        else if (instructions[i] == 0)
        {
            // Only count the first wait in a set of sequential waits
            if (!previous_instruction_was_wait)
            {
                wait_count += 1;
            }
            previous_instruction_was_wait = true;
        }
        else
        {
            previous_instruction_was_wait = false;
        }
    }

    // Check we don't have too many instructions to send
    if (words_to_send > max_words)
    {
        if (DEBUG)
        {
            // Divide by 2 to put it back in terms of "half_period reps" instructions
            // Subtract off two to remove the stop instruction from the count
            printf("Too many instructions to send to pseudoclock %d (%d > %d)\n", config->sm, (words_to_send - 2) / 2, max_words / 2);
        }
        return false;
    }

    // Check we don't have too many waits to send
    if (wait_count > max_waits)
    {
        if (DEBUG)
        {
            // subtract off one to remove the stop instruction from the wait count
            printf("Too many waits to send to pseudoclock %d (%d > %d)\n", config->sm, wait_count - 1, max_waits_per_pseudoclock);
        }
        return false;
    }

    if (DEBUG)
    {
        // word count:
        //      Divide by 2 to put it back in terms of "half_period reps" instructions
        //      Subtract off two to remove the stop instruction from the count
        // wait count:
        //      subtract off one to remove the stop instruction from the wait count
        printf("Will send %d instructions containing %d waits to pseudoclock %d\n", (words_to_send - 2) / 2, wait_count - 1, config->sm);
    }

    // Configure PIO Statemachine
    pio_pseudoclock_init(config->pio, config->sm, prog_offset, config->OUT_PIN, config->IN_PIN);

    // Update configuration with words/waits to send
    config->words_to_send = words_to_send;
    config->waits_to_send = wait_count;

    if (hwstart)
    {
        // send initial wait command (this precedes the DMA transfer)
        pio_sm_put_blocking(config->pio, config->sm, 0);
        pio_sm_put_blocking(config->pio, config->sm, 1); // This is ignored by the PIO code
    }

    // Configure automatic DMA transfer of instructions
    config->instructions_dma_channel = dma_claim_unused_channel(true);
    dma_channel_config instruction_c = dma_channel_get_default_config(config->instructions_dma_channel);

    // Set transfer request signal. Technically after this, dreq==sm will be true (for PIO0)
    // but we'll do this just incase the constants change. This sets the signal to be when
    // there is space in the PIO FIFO
    int instruction_dreq = 0;
    switch (config->sm)
    {
    case 0:
        instruction_dreq = DREQ_PIO1_TX0;
        break;
    case 1:
        instruction_dreq = DREQ_PIO1_TX1;
        break;
    case 2:
        instruction_dreq = DREQ_PIO1_TX2;
        break;
    case 3:
        instruction_dreq = DREQ_PIO1_TX3;
        break;
    default:
        break;
    }
    channel_config_set_dreq(&instruction_c, instruction_dreq);

    dma_channel_configure(
        config->instructions_dma_channel,      // The DMA channel
        &instruction_c,                        // DMA channel config
        &config->pio->txf[config->sm],         // Write address to the PIO TX FIFO
        &instructions[config->sm * max_words], // Read address to the instruction array
        words_to_send,                         // How many values to transfer
        true                                   // Start immediately
    );

    // Configure automatic transfer of wait lengths
    config->waits_dma_channel = dma_claim_unused_channel(true);
    dma_channel_config waits_c = dma_channel_get_default_config(config->waits_dma_channel);

    // Set transfer request signal. Technically after this, dreq==sm will be true (for PIO0)
    // but we'll do this just incase the constants change. This sets the signal to be when
    // there is space in the PIO FIFO
    int waits_dreq = 0;
    switch (config->sm)
    {
    case 0:
        waits_dreq = DREQ_PIO1_RX0;
        break;
    case 1:
        waits_dreq = DREQ_PIO1_RX1;
        break;
    case 2:
        waits_dreq = DREQ_PIO1_RX2;
        break;
    case 3:
        waits_dreq = DREQ_PIO1_RX3;
        break;
    default:
        break;
    }
    channel_config_set_dreq(&waits_c, waits_dreq);

    // change the default to increment write address and leave read constant
    channel_config_set_read_increment(&waits_c, false);
    channel_config_set_write_increment(&waits_c, true);

    dma_channel_configure(
        config->waits_dma_channel,      // The DMA channel
        &waits_c,                       // DMA channel config
        &waits[config->sm * max_waits], // write address to the waits array
        &config->pio->rxf[config->sm],  // Read address from the PIO RX FIFO
        wait_count,                     // How many values to transfer
        true                            // Start immediately
    );

    config->configured = true;

    return true;
}

void process_waits(pseudoclock_config *config, int max_waits_per_pseudoclock)
{
    // Note: We are not doing this right now because I want to distinguish
    // between triggering just as the timeout expires (so still jumping on
    // pin high) and the counter running out (and the assembly code falling
    // into waitdone without an external trigger). This is because the
    // latter is technically 1 clock cycle longer than the former, and so 
    // the counter will naturally demonstrate that.

    // Now process the read waits (but ignore the last one that was the stop signal)
    int max_waits = (max_waits_per_pseudoclock + 1);
    for (int i = config->sm * max_waits; i < (config->sm * max_waits) + config->waits_to_send - 1; i++)
    {
        // Check for timeout wait.
        // This integer equals 2^32-1 - aka the biggest number and what happens when you subtract one from 0 (it wraps around)
        // Note that we do not need to
        if (waits[i] == 4294967295)
        {
            waits[i] = 0; // decrement happens regardless of jump condition in PIO program.
        }
    }
}

void free_pseudoclock_pio_sm(pseudoclock_config *config)
{

    if (get_status() == ABORTING)
    {
        // Abort DMA transfer if we're aborting the shot
        dma_channel_abort(config->instructions_dma_channel);
        dma_channel_abort(config->waits_dma_channel);

        // Drain the FIFOs
        if (DEBUG)
        {
            printf("Draining instruction FIFO\n");
        }
        pio_sm_drain_tx_fifo(config->pio, config->sm);
        if (DEBUG)
        {
            printf("Draining wait FIFO\n");
        }
        // drain rx fifo
        while (pio_sm_get_rx_fifo_level(config->pio, config->sm) > 0)
        {
            pio_sm_get(config->pio, config->sm);
        }
        if (DEBUG)
        {
            printf("Pseudoclock program aborted\n");
        }
    }

    // Free the DMA channels
    dma_channel_unclaim(config->instructions_dma_channel);
    dma_channel_unclaim(config->waits_dma_channel);

    if (DEBUG)
    {
        printf("Draining TX FIFO\n");
    }
    // drain the tx FIFO to be safe
    pio_sm_drain_tx_fifo(config->pio, config->sm);

    // release the state machine
    pio_sm_unclaim(config->pio, config->sm);
}

// void rearrange_instructions(int old_num, int new_num)
// {
//     // reset all waits
//     for (int i = 0; i < max_waits + 4; i++)
//     {
//         waits[i] = 0;
//     }

//     int old_max_words = max_instructions*2/old_num+2;
//     int new_max_words = max_instructions*2/new_num+2;
//     // move instructions
//     if (old_num < new_num)
//     {
//         // Move instructions
//         for (int i = 0; i < new_num; i++)
//         {
//             for (int j = i * new_max_words; j < ((i + 1) * new_max_words); j += 1)
//             {
//                 instructions[j] = instructions[old_max_words*i+j];
//             }

//             // Ensure last instructions in first pseudoclock are 0
//             instructions[(i+1)*new_max_words-2] = 0;
//             instructions[(i+1)*new_max_words-1] = 0;

//             // Null out old instruction set
//             for (int j = i* old_max_words; j < ((i + 1) * old_max_words); j += 1)
//             {
//                 instructions[j] = 0;
//             }
//         }
//     }
//     else
//     {

//     }
// }

void core1_entry()
{
    // PIO initialisation
    uint offset = pio_add_program(pio1, &pseudoclock_program);

    // announce we are ready
    multicore_fifo_push_blocking(0);

    while (true)
    {
        // wait for message from main core
        uint32_t hwstart = multicore_fifo_pop_blocking();

        // Initialise configs
        pseudoclock_config pseudoclock_configs[4];
        bool success = true;
        for (int i = 0; i < num_pseudoclocks_in_use; i++)
        {
            pseudoclock_configs[i].pio = pio1;
            pseudoclock_configs[i].sm = i;
            pseudoclock_configs[i].OUT_PIN = OUT_PINS[i];
            pseudoclock_configs[i].IN_PIN = IN_PINS[i];
            success = configure_pseudoclock_pio_sm(&pseudoclock_configs[i], offset, hwstart, max_instructions / num_pseudoclocks_in_use, max_waits / num_pseudoclocks_in_use);
            if (!success)
            {
                if (DEBUG)
                {
                    printf("Failed to configure pseudoclock %d\n. Aborting.", i);
                }
                break;
            }
        }

        if (!success)
        {
            set_status(ABORTING);
            for (int i = 0; i < num_pseudoclocks_in_use; i++)
            {
                if (pseudoclock_configs[i].configured)
                {
                    free_pseudoclock_pio_sm(&pseudoclock_configs[i]);
                }
            }
            set_status(ABORTED);
            if (DEBUG)
            {
                printf("Core1 loop ended\n");
            }
            continue;
        }

        // Check that this shot has not been aborted already
        if (get_status() == TRANSITION_TO_RUNNING)
        {
            // update the status
            set_status(RUNNING);

            // Start the PIO SMs together as well as synchronising the clocks
            pio_enable_sm_mask_in_sync(pio1, ((1<<num_pseudoclocks_in_use)-1));

            // Wait for DMA transfers to finish
            for (int i = 0; i < num_pseudoclocks_in_use; i++)
            {
                if (pseudoclock_configs[i].configured)
                {
                    if (DEBUG)
                    {
                        printf("Tight loop for pseudoclock %d beginning\n", i);
                    }
                    while (dma_channel_is_busy(pseudoclock_configs[i].instructions_dma_channel) && get_status() != ABORT_REQUESTED)
                    {
                        tight_loop_contents();
                    }
                    if (DEBUG)
                    {
                        printf("Tight loop for pseudoclock waits %d beginning\n", i);
                    }
                    while (dma_channel_is_busy(pseudoclock_configs[i].waits_dma_channel) && get_status() != ABORT_REQUESTED)
                    {
                        tight_loop_contents();
                    }
                    if (DEBUG)
                    {
                        printf("Tight loops done for pseudoclock %d\n", i);
                    }
                }
            }
        }

        // If we aborted, update the status to acknowledge the abort
        if (get_status() == ABORT_REQUESTED)
        {
            if (DEBUG)
            {
                printf("Aborting pseudoclock program\n");
            }

            set_status(ABORTING);
        }
        // otherwise put in the transition to stop state
        else
        {
            if (DEBUG)
            {
                printf("Pseudoclock program complete\n");
            }

            set_status(TRANSITION_TO_STOP);

            // Note: We are not doing this right now because I want to distinguish
            // between triggering just as the timeout expires (so still jumping on
            // pin high) and the counter running out (and the assembly code falling
            // into waitdone without an external trigger). This is because the
            // latter is technically 1 clock cycle longer than the former, and so 
            // the counter will naturally demonstrate that.
            //
            // for (int i = 0; i < num_pseudoclocks_in_use; i++)
            // {
            //     if (pseudoclock_configs[i].configured)
            //     {
            //         process_waits(&pseudoclock_configs[i], max_waits / num_pseudoclocks_in_use);
            //     }
            // }
        }

        // cleanup
        for (int i = 0; i < num_pseudoclocks_in_use; i++)
        {
            if (pseudoclock_configs[i].configured)
            {
                free_pseudoclock_pio_sm(&pseudoclock_configs[i]);
            }
        }

        // Update the status
        if (get_status() == ABORTING)
        {
            set_status(ABORTED);
        }
        else
        {
            set_status(STOPPED);
        }

        if (DEBUG)
        {
            printf("Core1 loop ended\n");
        }
    }
}

void readline()
{
    int i = 0;
    char c;
    int crfound = 0;
    while (true)
    {
        char c = getchar();
        if (c == '\r')
        {
            crfound = 1;
        }
        else if (c == '\n')
        {
            if (crfound == 1)
            {
                readstring[i] = '\0';
                return;
            }
            else
            {
                readstring[i] = '\n';
                i++;
            }
        }
        else if (crfound)
        {
            crfound = 0;
            readstring[i] = '\r';
            i++;
            readstring[i] = c;
            i++;
        }
        else
        {
            readstring[i] = c;
            i++;
        }
    }
}

void configure_gpio()
{
    // initialise output pin. Needs to be done after state machine has run
    for (int i = 0; i < num_pseudoclocks_in_use; i++)
    {
        gpio_init(OUT_PINS[i]);
        gpio_set_dir(OUT_PINS[i], GPIO_OUT);
    }
}

void measure_freqs(void)
{
    // From https://github.com/raspberrypi/pico-examples under BSD-3-Clause License
    uint f_pll_sys = frequency_count_khz(CLOCKS_FC0_SRC_VALUE_PLL_SYS_CLKSRC_PRIMARY);
    uint f_pll_usb = frequency_count_khz(CLOCKS_FC0_SRC_VALUE_PLL_USB_CLKSRC_PRIMARY);
    uint f_rosc = frequency_count_khz(CLOCKS_FC0_SRC_VALUE_ROSC_CLKSRC);
    uint f_clk_sys = frequency_count_khz(CLOCKS_FC0_SRC_VALUE_CLK_SYS);
    uint f_clk_peri = frequency_count_khz(CLOCKS_FC0_SRC_VALUE_CLK_PERI);
    uint f_clk_usb = frequency_count_khz(CLOCKS_FC0_SRC_VALUE_CLK_USB);
    uint f_clk_adc = frequency_count_khz(CLOCKS_FC0_SRC_VALUE_CLK_ADC);
    uint f_clk_rtc = frequency_count_khz(CLOCKS_FC0_SRC_VALUE_CLK_RTC);

    printf("pll_sys = %dkHz\n", f_pll_sys);
    printf("pll_usb = %dkHz\n", f_pll_usb);
    printf("rosc = %dkHz\n", f_rosc);
    printf("clk_sys = %dkHz\n", f_clk_sys);
    printf("clk_peri = %dkHz\n", f_clk_peri);
    printf("clk_usb = %dkHz\n", f_clk_usb);
    printf("clk_adc = %dkHz\n", f_clk_adc);
    printf("clk_rtc = %dkHz\n", f_clk_rtc);
}

void resus_callback(void)
{
    // Switch back to internal clock with the dfeault frequency
    set_sys_clock_khz(100 * MHZ / 1000, false);

    // kill external clock pins
    gpio_set_function(20, GPIO_FUNC_NULL);
    gpio_set_function(22, GPIO_FUNC_NULL);

    // update clock status
    clock_status = INTERNAL;

    // Reconfigure uart as clocks have changed
    stdio_init_all();
    if (DEBUG)
    {
        printf("Resus event fired\n");
    }

    // Wait for uart output to finish
    uart_default_tx_wait_blocking();
}

void loop()
{
    readline();
    int local_status = get_status();
    if (strncmp(readstring, "status", 6) == 0)
    {
        printf("run-status:%d clock-status:%d\n", local_status, clock_status);
    }
    else if (strncmp(readstring, "debug on", 8) == 0)
    {
        DEBUG = 1;
        printf("ok\n");
    }
    else if (strncmp(readstring, "debug off", 9) == 0)
    {
        DEBUG = 0;
        printf("ok\n");
    }
    else if (strncmp(readstring, "getfreqs", 8) == 0)
    {
        measure_freqs();
        printf("ok\n");
    }
    else if (strncmp(readstring, "abort", 5) == 0)
    {
        if (local_status != RUNNING && local_status != TRANSITION_TO_RUNNING)
        {
            printf("Can only abort when status is 1 or 2 (transitioning to running or running)");
        }
        else
        {
            // force output low first, this should take control from the state machine
            // and prevent it from changing the output pin state erroneously as we drain the fifo
            set_status(ABORT_REQUESTED);
            configure_gpio();
            for (int i = 0; i < num_pseudoclocks_in_use; i++)
            {
                gpio_put(OUT_PINS[i], 0);
            }
            // Should be done!
            printf("ok\n");
        }
    }
    // Prevent manual mode commands from running during buffered execution
    else if (local_status != ABORTED && local_status != STOPPED)
    {
        printf("Cannot execute command %s during buffered execution. Check status first and wait for it to return 0 or 5 (stopped or aborted).\n", readstring);
    }
    // Set number of pseudoclocks
    else if (strncmp(readstring, "setnumpseudoclocks", 17) == 0)
    {
        unsigned int num_pseudoclocks;
        int parsed = sscanf(readstring, "%*s %u %u", &num_pseudoclocks);
        if (parsed < 1)
        {
            printf("invalid request\n");
        }
        else if (num_pseudoclocks < 1 || num_pseudoclocks > 4)
        {
            printf("The number of pseudoclocks must be between 1 and 4 (inclusive)\n");
        }
        else
        {
            // TODO: be cleverer here and rearrange instructions
            // rearrange_instructions(num_pseudoclocks_in_use, num_pseudoclocks);

            // reset waits
            for (int i = 0; i < max_waits + 4; i++)
            {
                waits[i] = 0;
            }
            // reset instructions
            for (int i = 0; i < max_instructions * 2 + 8; i++)
            {
                instructions[i] = 0;
            }
            num_pseudoclocks_in_use = num_pseudoclocks;
            printf("ok\n");
        }
    }
    else if (strncmp(readstring, "setinpin", 8) == 0)
    {
        unsigned int pin_no;
        unsigned int pseudoclock;
        int parsed = sscanf(readstring, "%*s %u %u", &pseudoclock, &pin_no);
        if (parsed < 2)
        {
            printf("invalid request\n");
        }
        else if (pseudoclock < 0 || pseudoclock > 3)
        {
            printf("The specified pseudoclock must be between 0 and 3 (inclusive)\n");
        }
        else if (pin_no == OUT_PINS[0] || pin_no == OUT_PINS[1] || pin_no == OUT_PINS[2] || pin_no == OUT_PINS[3])
        {
            printf("IN pin cannot be the same as one of the OUT pins\n");
        }
        else if (pin_no < 0 || pin_no > 19)
        {
            printf("IN pin must be between 0 and 19 (inclusive)\n");
        }
        else if (pin_no == IN_PINS[pseudoclock])
        {
            printf("ok\n");
        }
        else if (pin_no == IN_PINS[0] || pin_no == IN_PINS[1] || pin_no == IN_PINS[2] || pin_no == IN_PINS[3])
        {
            printf("IN pin cannot be the same as one of the other IN pins\n");
        }
        else
        {
            IN_PINS[pseudoclock] = pin_no;
            printf("ok\n");
        }
    }
    else if (strncmp(readstring, "setoutpin", 9) == 0)
    {
        unsigned int pin_no;
        unsigned int pseudoclock;
        int parsed = sscanf(readstring, "%*s %u %u", &pseudoclock, &pin_no);
        if (parsed < 2)
        {
            printf("invalid request\n");
        }
        else if (pseudoclock < 0 || pseudoclock > 3)
        {
            printf("The specified pseudoclock must be between 0 and 3 (inclusive)\n");
        }
        else if (pin_no == IN_PINS[0] || pin_no == IN_PINS[1] || pin_no == IN_PINS[2] || pin_no == IN_PINS[3])
        {
            printf("OUT pin cannot be the same as one of the IN pins\n");
        }
        else if (pin_no != 25 && (pin_no < 0 || pin_no > 19))
        {
            printf("OUT pin must be between 0 and 19 (inclusive) or 25 (LED for debugging)\n");
        }
        else if (pin_no == OUT_PINS[pseudoclock])
        {
            printf("ok\n");
        }
        else if (pin_no == OUT_PINS[0] || pin_no == OUT_PINS[1] || pin_no == OUT_PINS[2] || pin_no == OUT_PINS[3])
        {
            printf("OUT pin cannot be the same as one of the other OUT pins\n");
        }
        else
        {
            OUT_PINS[pseudoclock] = pin_no;
            printf("ok\n");
        }
    }
    else if (strncmp(readstring, "setclock", 8) == 0)
    {
        unsigned int src;     // 0 = internal, 1=GPIO pin 20, 2=GPIO pin 22
        unsigned int freq;    // in Hz (up to 133 MHz)
        int parsed = sscanf(readstring, "%*s %u %u", &src, &freq);
        if (parsed < 2)
        {
            printf("invalid request\n");
        }
        else
        {
            if (DEBUG)
            {
                printf("Got request mode=%u, freq=%u MHz\n", src, freq / MHZ);
            }
            if (src > 2)
            {
                printf("invalid request\n");
            }
            else
            {
                // Do validation checking on values provided
                if (freq > 133 * MHZ)
                {
                    printf("Invalid clock frequency specified\n");
                    return;
                }

                // Set new clock frequency
                if (src == 0)
                {
                    if (set_sys_clock_khz(freq / 1000, false))
                    {
                        printf("ok\n");
                        clock_status = INTERNAL;
                    }
                    else
                    {
                        printf("Failure. Cannot exactly achieve that clock frequency.");
                    }
                }
                else
                {
                    clock_configure_gpin(clk_sys, (src == 2 ? 22 : 20), freq, freq);
                    // update clock status
                    clock_status = EXTERNAL;
                    printf("ok\n");
                }
            }
        }
    }
    // TODO: update this to support pseudoclock selection
    else if (strncmp(readstring, "getwait", 7) == 0)
    {
        unsigned int addr;
        unsigned int pseudoclock;
        int parsed = sscanf(readstring, "%*s %u %u", &pseudoclock, &addr);
        int waits_per_pseudoclock = (max_waits / num_pseudoclocks_in_use) + 1;
        if (parsed < 2)
        {
            printf("invalid request\n");
        }
        else if (pseudoclock < 0 || pseudoclock > 3)
        {
            printf("The specified pseudoclock must be between 0 and 3 (inclusive)\n");
        }
        else if (addr >= waits_per_pseudoclock)
        {
            printf("invalid address\n");
        }
        else
        {
            unsigned int wait_remaining = waits[pseudoclock * waits_per_pseudoclock + addr];
            // don't multiply the -1 wraparound of the unsigned int - this means a
            // wait timed out.
            if (wait_remaining != 4294967295)
            {
                // Note that these are not the lengths of the waits, but how many base (system) clock ticks were left
                // before timeout. 0 = timeout. a wait with a timeout of 8, and a value reported here as 2, means the
                // wait was 6 clock ticks long.
                //
                // We multiply by two here to counteract the divide by two when storing (see below)
                wait_remaining *= 2;
            }
            printf("%u\n", wait_remaining);
        }
    }
    else if (strncmp(readstring, "hwstart", 7) == 0)
    {
        // Force output low in case it was left high
        for (int i = 0; i < num_pseudoclocks_in_use; i++)
        {
            gpio_put(OUT_PINS[i], 0);
        }
        // Notify state machine to start
        multicore_fifo_push_blocking(1);
        // update status
        set_status(TRANSITION_TO_RUNNING);
        printf("ok\n");
    }
    else if ((strncmp(readstring, "start", 5) == 0))
    {
        // Force output low in case it was left high
        for (int i = 0; i < num_pseudoclocks_in_use; i++)
        {
            gpio_put(OUT_PINS[i], 0);
        }
        // Notify state machine to start
        multicore_fifo_push_blocking(0);
        // update status
        set_status(TRANSITION_TO_RUNNING);
        printf("ok\n");
    }
    // TODO: update this to support pseudoclock selection
    else if (strncmp(readstring, "set ", 4) == 0)
    {
        unsigned int addr;
        unsigned int half_period;
        unsigned int reps;
        unsigned int pseudoclock;
        int parsed = sscanf(readstring, "%*s %u %u %u %u", &pseudoclock, &addr, &half_period, &reps);
        int address_offset = pseudoclock * (max_instructions * 2 / num_pseudoclocks_in_use + 2);
        if (parsed < 4)
        {
            printf("invalid request\n");
        }
        else if (pseudoclock < 0 || pseudoclock > 3)
        {
            printf("The specified pseudoclock must be between 0 and 3 (inclusive)\n");
        }
        else if (addr >= max_instructions)
        {
            printf("invalid address\n");
        }
        else if (reps == 0)
        {
            // This indicates either a stop or a wait instruction
            instructions[address_offset + addr * 2] = 0;
            if (half_period == 0)
            {
                // It's a stop instruction
                instructions[address_offset + addr * 2 + 1] = 0;
                printf("ok\n");
            }
            else if (half_period >= 6)
            {
                // It's a wait instruction:
                // The half period contains the number of ASM wait loops to wait for before continuing.
                // There are 4 clock cycles of delay between ending the previous instruction and being 
                // ready to detect the trigger to end the wait. So we also subtract these off to ensure
                // the timeout is accurate.
                // The wait loop conatins two ASM instructions, so we divide by 2 here.
                instructions[address_offset + addr * 2 + 1] = (half_period - 4) / 2;
                printf("ok\n");
            }
            else
            {
                printf("invalid request\n");
            }
        }
        else if (half_period < (non_loop_path_length))
        {
            printf("half-period too short\n");
        }
        else if (reps < 1)
        {
            printf("reps must be at least one\n");
        }
        else
        {
            instructions[address_offset + addr * 2] = reps;
            instructions[address_offset + addr * 2 + 1] = half_period - non_loop_path_length;
            printf("ok\n");
        }
    }
    else if (strncmp(readstring, "get ", 4) == 0)
    {
        unsigned int addr;
        unsigned int pseudoclock;
        int parsed = sscanf(readstring, "%*s %u %u", &pseudoclock, &addr);
        int address_offset = pseudoclock * (max_instructions * 2 / num_pseudoclocks_in_use + 2);
        if (parsed < 2)
        {
            printf("invalid request\n");
        }
        else if (pseudoclock < 0 || pseudoclock > 3)
        {
            printf("The specified pseudoclock must be between 0 and 3 (inclusive)\n");
        }
        else if (addr >= max_instructions)
        {
            printf("invalid address\n");
        }
        else
        {
            uint half_period = instructions[address_offset + addr * 2 + 1];
            uint reps = instructions[address_offset + addr * 2];
            if (reps != 0)
            {
                half_period += non_loop_path_length;
            }
            else
            {
                // account for wait loop being 2 ASM instructions long
                half_period = half_period * 2;
                // If not a stop instruction
                if (half_period != 0)
                {
                    // acount for 3 ASM instructions between end of previous pseudoclock instruction and start of wait loop
                    half_period += 3;
                }
            }
            printf("%u %u\n", half_period, reps);
        }
    }
    else if (strncmp(readstring, "go high", 7) == 0)
    {
        unsigned int pseudoclock;
        int parsed = sscanf(readstring, "%*s %*s %u", &pseudoclock);
        if (parsed < 1)
        {
            printf("invalid request\n");
        }
        else if (pseudoclock < 0 || pseudoclock > 3)
        {
            printf("The specified pseudoclock must be between 0 and 3 (inclusive)\n");
        }
        else
        {
            configure_gpio();
            gpio_put(OUT_PINS[pseudoclock], 1);
            printf("ok\n");
        }
    }
    else if (strncmp(readstring, "go low", 6) == 0)
    {
        unsigned int pseudoclock;
        int parsed = sscanf(readstring, "%*s %*s %u", &pseudoclock);
        if (parsed < 1)
        {
            printf("invalid request\n");
        }
        else if (pseudoclock < 0 || pseudoclock > 3)
        {
            printf("The specified pseudoclock must be between 0 and 3 (inclusive)\n");
        }
        else
        {
            configure_gpio();
            gpio_put(OUT_PINS[pseudoclock], 0);
            printf("ok\n");
        }
    }
    else
    {
        printf("invalid request: %s\n", readstring);
    }
}

int main()
{
    DEBUG = 0;
    // Initial config for output pins
    OUT_PINS[0] = 9;
    OUT_PINS[1] = 11;
    OUT_PINS[2] = 13;
    OUT_PINS[3] = 15;
    IN_PINS[0] = 0;
    IN_PINS[1] = 2;
    IN_PINS[2] = 4;
    IN_PINS[3] = 6;
    // start with only one in use
    num_pseudoclocks_in_use = 1;

    // initialise the status mutex
    mutex_init(&status_mutex);

    // configure resus callback that will reconfigure the clock for us
    // either when we change clock settings or when the clock fails (if external)
    clocks_enable_resus(&resus_callback);

    // initial clock configuration
    set_sys_clock_khz(100 * MHZ / 1000, true);

    // Temp output 48MHZ clock for debug
    clock_gpio_init(21, CLOCKS_CLK_GPOUT0_CTRL_AUXSRC_VALUE_CLK_USB, 1);

    stdio_init_all();

    multicore_launch_core1(core1_entry);
    multicore_fifo_pop_blocking();

    while (true)
    {
        loop();
    }
    return 0;
}
