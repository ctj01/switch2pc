/*
 * sdk: a library module. Exports a function and a data symbol that `main`
 * imports through its dynamic symbol table, exactly how a game split into
 * main + sdk + subsdk* NSOs is linked by nnrtld.
 */

#include <stdint.h>

__attribute__((visibility("default"))) const char *sdk_message = "sdk module says hi";

__attribute__((visibility("default"))) int sdk_add(int a, int b) {
    return a + b;
}

__attribute__((visibility("default"))) uint64_t sdk_tick(void) {
    register uint64_t x0 __asm__("x0");
    __asm__ volatile("svc #0x1E" : "=r"(x0) : : "x1", "x2", "x3", "memory");
    return x0;
}
