#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
typedef unsigned int uint;
#define PARAM_ASSERTIONS_ENABLED(x) 0
#define panic(...) ((void)0)
#define valid_params_if(x,y) ((void)0)
#define pio_encode_delay _pio_encode_delay
#define pio_encode_sideset _pio_encode_sideset
#define pio_encode_sideset_opt _pio_encode_sideset_opt
#define pio_encode_jmp _pio_encode_jmp
#define pio_encode_jmp_not_x _pio_encode_jmp_not_x
#define pio_encode_jmp_x_dec _pio_encode_jmp_x_dec
#define pio_encode_jmp_not_y _pio_encode_jmp_not_y
#define pio_encode_jmp_y_dec _pio_encode_jmp_y_dec
#define pio_encode_jmp_x_ne_y _pio_encode_jmp_x_ne_y
#define pio_encode_jmp_pin _pio_encode_jmp_pin
#define pio_encode_jmp_not_osre _pio_encode_jmp_not_osre
#define pio_encode_wait_gpio _pio_encode_wait_gpio
#define pio_encode_wait_pin _pio_encode_wait_pin
#define pio_encode_wait_irq _pio_encode_wait_irq
#define pio_encode_in _pio_encode_in
#define pio_encode_out _pio_encode_out
#define pio_encode_push _pio_encode_push
#define pio_encode_pull _pio_encode_pull
#define pio_encode_mov _pio_encode_mov
#define pio_encode_mov_not _pio_encode_mov_not
#define pio_encode_mov_reverse _pio_encode_mov_reverse
#define pio_encode_irq_set _pio_encode_irq_set
#define pio_encode_irq_wait _pio_encode_irq_wait
#define pio_encode_irq_clear _pio_encode_irq_clear
#define pio_encode_set _pio_encode_set
#define pio_encode_nop _pio_encode_nop
#include "hardware/pio_instructions.h"
#include "../src/dpwm.h"
int main(void){
  uint16_t ref[11] = {
    pio_encode_pull(false, false), pio_encode_mov(pio_x, pio_osr),
    pio_encode_set(pio_pins, 3), pio_encode_out(pio_y, 10), pio_encode_jmp_y_dec(4),
    pio_encode_out(pio_pins, 2), pio_encode_out(pio_y, 10), pio_encode_jmp_y_dec(7),
    pio_encode_set(pio_pins, 0), pio_encode_out(pio_y, 10), pio_encode_jmp_y_dec(10) };
  uint16_t dref[8] = {
    pio_encode_pull(false, false), pio_encode_mov(pio_x, pio_osr), pio_encode_mov(pio_y, pio_x),
    pio_encode_set(pio_pins, 3), pio_encode_jmp_y_dec(4), pio_encode_mov(pio_y, pio_x),
    pio_encode_set(pio_pins, 0), pio_encode_jmp_y_dec(7) };
  int bad = 0;
  for (int i = 0; i < 11; i++) {
    printf("%2d ref %04x mine %04x %s\n", i, ref[i], dpwm_program[i], ref[i]==dpwm_program[i]?"":"<-- MISMATCH");
    bad |= ref[i] != dpwm_program[i];
  }
  for (int i = 0; i < 8; i++) {
    printf("diag %d ref %04x mine %04x %s\n", i, dref[i], diag_program[i], dref[i]==diag_program[i]?"":"<-- MISMATCH");
    bad |= dref[i] != diag_program[i];
  }
  return bad;
}
