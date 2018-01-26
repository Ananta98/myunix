#ifndef ISR_H
#define ISR_H 1

#include <stdint.h>
#include <cpu.h>

void isr_init();

typedef void *(*isr_handler)(registers_t *regs);

void isr_set_handler(uint8_t i, isr_handler handler);

#endif