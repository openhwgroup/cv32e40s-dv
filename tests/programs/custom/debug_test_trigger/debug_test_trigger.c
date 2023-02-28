/*
**
** Copyright 2022 OpenHW Group
**
** Licensed under the Solderpad Hardware Licence, Version 2.0 (the "License");
** you may not use this file except in compliance with the License.
** You may obtain a copy of the License at
**
**     https://solderpad.org/licenses/
**
** Unless required by applicable law or agreed to in writing, software
** distributed under the License is distributed on an "AS IS" BASIS,
** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
** See the License for the specific language governing permissions and
** limitations under the License.
**
*******************************************************************************
** Debug trigger test
*******************************************************************************
*/

#include <stdio.h>
#include <stdint.h>
#include "corev_uvmt.h"

// MUST be 31 or less (bit position-1 in result array determines test pass/fail
// status, thus we are limited to 31 tests with this construct.
#define NUM_TESTS 10
#define FAIL    1
#define SUCCESS 0

#define DEBUG_REQ_CONTROL_REG *(volatile int *) CV_VP_DEBUG_CONTROL_BASE

#define DEBUG_SEL_IDLE 0
#define DEBUG_SEL_DISABLE_TRIGGER 1
#define DEBUG_SEL_SETUP_TRIGGER 2
#define DEBUG_SEL_CLEAR_TDATA2 3

#define DEBUG_LOOPBREAK_NONE    0
#define DEBUG_LOOPBREAK_TDATA1  1
#define DEBUG_LOOPBREAK_TDATA2  2
#define DEBUG_LOOPBREAK_DPCINCR 3

#define TRIGGER_NONE     0
#define TRIGGER_BYTE     1
#define TRIGGER_HALFWORD 2
#define TRIGGER_WORD     3

                               // Place in debugger section
void _debugger_start(void)     __attribute__((section(".debugger"))) __attribute__((naked));
void _debugger(void)           __attribute__((section(".debugger")));

void handle_illegal_insn(void) __attribute__ ((naked));
extern void end_handler_incr_mepc(void);

                                                 // Ensure function call is executed, and that fist instruction is the one we expect
volatile void trigger_code_nop(void)             __attribute__((optimize("O0"))) __attribute__((naked));
volatile void trigger_code_ebreak(void)          __attribute__((optimize("O0"))) __attribute__((naked));
volatile void trigger_code_cebreak(void)         __attribute__((optimize("O0"))) __attribute__((naked));
volatile void trigger_code_branch_insn(void)     __attribute__((optimize("O0"))) __attribute__((naked));
volatile void trigger_code_illegal_insn(void)    __attribute__((optimize("O0"))) __attribute__((naked));
volatile void trigger_code_multicycle_insn(void) __attribute__((optimize("O0"))) __attribute__((naked));


int  test_execute_trigger(void);
int  test_load_trigger(void);

volatile uint32_t mcontrol_val;
volatile uint32_t trigger_address;
volatile uint32_t trigger_address_offset;
volatile int      trigger_load;
volatile int      trigger_store;
volatile int      trigger_execute;

volatile uint32_t num_triggers;
volatile uint32_t trigger_sel;

volatile int debug_sel;
volatile int debug_break_loop;
volatile int debug_entry_status;

volatile uint32_t illegal_insn_status;

volatile uint8_t  some_data_bytes[4]     = {0xC0, 0xFF, 0xEB, 0xEE};
volatile uint16_t some_data_halfwords[2] = {0xDEAD, 0xBEEF};
volatile uint32_t some_data_word         = 0xC0DECAFE;

void handle_illegal_insn (void) {
  //printf("  Illegal insn\n");
    illegal_insn_status = 1;
    end_handler_incr_mepc(); // Let BSP handle mepc inrements, stack restore and mret
}

void _debugger_start(void) {
  __asm__ volatile (R"(
    # Store return address and saved registers

      cm.push {ra, s0-s11}, -64

    # Execute _debugger() function
      call ra, _debugger

    # Restore return address and saved registers
      cm.pop {ra, s0-s11}, 64

    # Exit debug mode
      dret
  )");
}

void _debugger (void) {
  volatile uint32_t t0;
  volatile uint32_t t1;
  volatile uint32_t t2;

  printf("  Entered debug\n");

  debug_entry_status = 1;

  switch (debug_sel) {

    case DEBUG_SEL_DISABLE_TRIGGER:
      if (trigger_load != TRIGGER_NONE) {
        __asm__ volatile ("csrci tdata1, (1 << 0)"); // Clear load bit
        printf("    Disabling trigger by clearing TDATA1->LOAD\n");
      }
      if (trigger_store != TRIGGER_NONE) {
        __asm__ volatile ("csrci tdata1, (1 << 1)"); // Clear store bit
        printf("    Disabling trigger by clearing TDATA1->STORE\n");
      }
      if (trigger_execute != TRIGGER_NONE) {
        __asm__ volatile ("csrci tdata1, (1 << 2)"); // Clear execute bit
        printf("    Disabling trigger by clearing TDATA1->EXECUTE\n");
      }

    break;

    case DEBUG_SEL_SETUP_TRIGGER: // Set up trigger
      // Load tdata config csrs
      printf("    Setting up triggers\n      csr_write: tdata1 = 0x%08lx\n      csr_write: tdata2 = 0x%08lx (0x%lx + 0x%lx)\n",
             mcontrol_val, (trigger_address + trigger_address_offset), trigger_address, trigger_address_offset);
      __asm__ volatile (R"(la   %[temp0],     mcontrol_val
                           lw   %[temp1],     0(%[temp0])
                           csrw tdata1, %[temp1]
                           la   s2,     trigger_address
                           lw   s0,     0(s2)
                           la   s2,     trigger_address_offset
                           lw   s1,     0(s2)
                           add  s0, s0, s1
                           csrw tdata2, s0)" : [temp0] "=r" (t0), [temp1] "=r" (t1), [temp2] "=r" (t2)  :: "s0", "s1", "s2");

    break;

    case DEBUG_SEL_CLEAR_TDATA2:
      __asm__ volatile ("csrwi tdata2, 0x0");
      printf("    Disabling trigger by clearing TDATA2\n");
    break;
  }

  switch (debug_break_loop) {
    case DEBUG_LOOPBREAK_NONE:
      break;
    case DEBUG_LOOPBREAK_TDATA1:
      debug_sel = DEBUG_SEL_DISABLE_TRIGGER;
      break;
    case DEBUG_LOOPBREAK_TDATA2:
      // Avoid re-triggering when returning to dpc
      debug_sel = DEBUG_SEL_CLEAR_TDATA2;
      break;
    case DEBUG_LOOPBREAK_DPCINCR:
      __asm__ volatile (R"(
        # Increment dpc to skip matched instruction
           csrr s0, dpc
           lb   s1, 0(s0)
           li   s2, 0x3
           and  s1, s1, s2
           bne  s1, s2, 1f
           addi s0, s0, 0x2
         1:addi s0, s0, 0x2
           csrw dpc, s0
      )" ::: "s0", "s1", "s2");
      printf("    Incrementing dpc\n");
      break;
  }
  return;
}

void disable_trigger () {
  debug_entry_status = 0;
  // Disable trigger after use
  debug_sel = DEBUG_SEL_DISABLE_TRIGGER;

  // Assert debug req
  DEBUG_REQ_CONTROL_REG = (CV_VP_DEBUG_CONTROL_DBG_REQ(0x1)        |
                           CV_VP_DEBUG_CONTROL_REQ_MODE(0x1)       |
                           CV_VP_DEBUG_CONTROL_PULSE_DURATION(0x8) |
                           CV_VP_DEBUG_CONTROL_START_DELAY(0xc8));
  // Wait for debug entry
  while (debug_entry_status == 0);
}


// The trigger code functions need at least 2 instructions as the first is skipped (dpc inrement)
// to ensure it is not executed before debug entry
volatile void trigger_code_nop() {
  __asm__ volatile (R"(nop
                       nop
                       ret)");
}
volatile void trigger_code_ebreak() {
  __asm__ volatile (R"(.4byte 0x00100073 # ebreak
                       nop
                       ret)");
}
volatile void trigger_code_cebreak() {
  __asm__ volatile (R"(c.ebreak
                       nop
                       ret)");
}
volatile void trigger_code_branch_insn() {
  __asm__ volatile (R"(beq t0, t0, trigger_code_ebreak
                       nop
                       ret)");
}
volatile void trigger_code_illegal_insn() {
  __asm__ volatile (R"(dret
                       nop
                       ret)");
}
volatile void trigger_code_multicycle_insn() {
  __asm__ volatile (R"(mulhsu t0, t0, t1
                       nop
                       ret)");
}

int trigger_test (int setup, int expect_trigger_match, uint32_t trigger_addr) {

  //printf ("\ntrigger_test():\n");

  debug_entry_status = 0;
  trigger_address = trigger_addr;
  if (setup) {
    debug_sel = DEBUG_SEL_SETUP_TRIGGER;

    // Assert debug req
    DEBUG_REQ_CONTROL_REG = (CV_VP_DEBUG_CONTROL_DBG_REQ(0x1)        |
                             CV_VP_DEBUG_CONTROL_REQ_MODE(0x1)       |
                             CV_VP_DEBUG_CONTROL_PULSE_DURATION(0x8) |
                             CV_VP_DEBUG_CONTROL_START_DELAY(0xc8));
    // Wait for debug entry
    while (debug_entry_status == 0);
    debug_entry_status = 0;
  }

  debug_sel = DEBUG_SEL_IDLE;

  if (trigger_load == TRIGGER_BYTE) {
    __asm__ volatile (R"(lw s4, trigger_address
                         lb s3, 0(s4)          )" ::: "s3", "s4");
  }
  if (trigger_load == TRIGGER_HALFWORD) {
    __asm__ volatile (R"(lw s4, trigger_address
                         lh s3, 0(s4)          )" ::: "s3", "s4");
  }
  if (trigger_load == TRIGGER_WORD) {
    __asm__ volatile (R"(lw s4, trigger_address
                         lw s3, 0(s4)          )" ::: "s3", "s4");
  }
  if (trigger_store) {
    __asm__ volatile (R"(lw s4, trigger_address
                         sw s3, 0(s4)          )" ::: "s3", "s4");
  }
  if (trigger_execute) {
    __asm__ volatile (R"(lw   s4, trigger_address
                         jalr ra, s4             )" ::: "ra", "s4"); // Jump to triggered address
  }

  printf ("  Address match debug entry: %d (expected: %d)\n\n",
          debug_entry_status,   expect_trigger_match);
  return (debug_entry_status == expect_trigger_match) ? SUCCESS : FAIL;
}

int test_execute_trigger () {
  int retval = 0;

  //printf("\n\n\n --- Testing execute triggers ---\n\n");

  // Set up trigger
  mcontrol_val = (6 << 28 | // TYPE = 6
                  1 << 27 | // DMODE = 1
                  1 << 12 | // ACTION = Enter debug mode
                  0 << 7  | // MATCH = EQ
                  1 << 6  | // M = Match in machine mode
                  1 << 2 ); // EXECUTE = Match on instruction address

  // Attempt accessing tdata registers outside debug mode, should be ignored
  __asm__ volatile ("csrci tdata1, (1 << 2)");
  __asm__ volatile ("csrwi tdata2, 0");

  trigger_load    = 0;
  trigger_store   = 0;
  trigger_execute = 1;
  debug_break_loop = DEBUG_LOOPBREAK_TDATA2;
  // Check that executing trigger_code function does not trigger when it is not set up
  retval += trigger_test(0, 0, (uint32_t) &trigger_code_nop);

  // Check that clearing tdata2 prevents re-triggering upon return
  retval += trigger_test(1, 1, (uint32_t) &trigger_code_nop);

  // Check that executing trigger_code function does not trigger when it is disabled in tdata1
  retval += trigger_test(0, 0, (uint32_t) &trigger_code_nop);

  // Check that executing various instructions at the triggered address causes debug entry
  // and make sure it is not executed before entering debug
  debug_break_loop = DEBUG_LOOPBREAK_DPCINCR;
  retval += trigger_test(1, 1, (uint32_t) &trigger_code_nop);
  retval += trigger_test(1, 1, (uint32_t) &trigger_code_ebreak);
  retval += trigger_test(1, 1, (uint32_t) &trigger_code_cebreak);
  retval += trigger_test(1, 1, (uint32_t) &trigger_code_branch_insn);
  retval += trigger_test(1, 1, (uint32_t) &trigger_code_illegal_insn);
  retval += trigger_test(1, 1, (uint32_t) &trigger_code_multicycle_insn);

  return retval;
}

int test_load_trigger () {
  int retval = 0;
  printf("\n\n\n --- Testing load triggers ---\n\n");
  // Set up trigger
  mcontrol_val = (6 << 28 | // TYPE = 6
                  1 << 27 | // DMODE = 1
                  1 << 12 | // ACTION = Enter debug mode
                  0 << 7  | // MATCH = EQ
                  1 << 6  | // M = Match in machine mode
                  1 << 0 ); // LOAD = Match on load from data address

  trigger_load    = TRIGGER_WORD;
  trigger_store   = 0;
  trigger_execute = 0;

  debug_break_loop   = DEBUG_LOOPBREAK_TDATA2;
  retval += trigger_test(1, 1, (uint32_t) &some_data_word);

  debug_break_loop   = DEBUG_LOOPBREAK_DPCINCR;
  retval += trigger_test(1, 1, (uint32_t) &some_data_word);

  trigger_address_offset = 4;
  retval += trigger_test(1, 0, (uint32_t) &some_data_word);
  trigger_address_offset = -4;
  retval += trigger_test(1, 0, (uint32_t) &some_data_word);

  trigger_address_offset = 0;

  trigger_load    = TRIGGER_HALFWORD;
  retval += trigger_test(1, 1, (uint32_t) &some_data_halfwords[0]);
  retval += trigger_test(1, 1, (uint32_t) &some_data_halfwords[1]);

  trigger_load    = TRIGGER_BYTE;
  retval += trigger_test(1, 1, (uint32_t) &some_data_bytes[0]);
  retval += trigger_test(1, 1, (uint32_t) &some_data_bytes[1]);
  retval += trigger_test(1, 1, (uint32_t) &some_data_bytes[2]);
  retval += trigger_test(1, 1, (uint32_t) &some_data_bytes[3]);

  trigger_load    = TRIGGER_WORD;


  mcontrol_val |= 2 << 7; // Set MATCH = 2 (GEQ)

  debug_break_loop =   DEBUG_LOOPBREAK_TDATA1;

  // Loading from start of debug memory to avoid triggering on loads from other variables
  retval += trigger_test(1, 1, (uint32_t) &_debugger_start);
  trigger_address_offset = -4;
  retval += trigger_test(1, 1, (uint32_t) &_debugger_start);
  trigger_address_offset = 4;
  retval += trigger_test(1, 0, (uint32_t) &_debugger_start);

  disable_trigger();

  return retval;
}

uint32_t get_num_triggers() {

  __asm__ volatile (R"(
    # Check whether there are 0 triggers
    la   s3, illegal_insn_status
    li   s2, 0
    sw   s2, 0(s3)
    csrwi tselect, 0x0
  )" ::: "s2", "s3");

  if (illegal_insn_status) {
    num_triggers = 0;
  } else {
    __asm__ volatile (R"(
      csrwi tselect, 0x1
      csrwi tselect, 0x2
      csrwi tselect, 0x3
      csrr s2, tselect
      la   s3, num_triggers
      sw   s2, 0(s3)

      csrwi tselect, 0x0
    )" ::: "s2", "s3");

    num_triggers++;
  }

  printf ("NUM_TRIGGERS = %ld\n", num_triggers);

  return num_triggers;
}

int main(int argc, char *argv[])
{
  int status = 0;

  trigger_address_offset = 0;

  num_triggers = get_num_triggers();

  if (num_triggers > 0) {
    for (int i = 0; i < num_triggers; i++) {

      //debug_break_loop   = DEBUG_LOOPBREAK_DPCINCR;
      trigger_sel = i;
      printf ("csr_write: tselect = %ld", trigger_sel);
      __asm__ volatile (R"(lw        s2, trigger_sel
                           csrw tselect, s2         )" ::: "s2");

      status  = test_execute_trigger();
      status += test_load_trigger ();
      if (status != 0) {
        printf("Test 0 failed with status: %d\n", status);
        return status;
      }
    }
    printf("Finished \n");
    return status;
  } else {
    printf("Error: Tselect register does not exist (NUM_TRIGGERS=0 not supported in this test) \n");
    return 1;
  }
}