/* Standalone CPU component checks, not a gameplay recording or hardware proof.
 * Build with: gcc -O2 -I src/libs/cpu-z80 tests/test_pc_coverage.c
 *             src/libs/cpu-z80/z80.c -o test_pc_coverage.exe */
#include "z80.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct { uint8_t ram[65536]; unsigned reads, writes; } memory_t;
static uint8_t rd(z80_t *cpu, uint16_t a, int m1, void *data) {
    (void)cpu; (void)m1; memory_t *m=data; m->reads++; return m->ram[a];
}
static void wr(z80_t *cpu, uint16_t a, uint8_t v, void *data) {
    (void)cpu; memory_t *m=data; m->writes++; m->ram[a]=v;
}
static z80_t *create(memory_t *m) {
    return z80_create(rd,m,wr,m,NULL,NULL,NULL,NULL,NULL,NULL);
}
static int hit(z80_t *cpu, unsigned pc) {
    return !!(cpu->coverage_bitmap[pc>>3] & (1u<<(pc&7)));
}
static void clear(z80_t *cpu) {
    memset(cpu->coverage_bitmap,0,sizeof(cpu->coverage_bitmap));
    cpu->coverage_unique=0;cpu->coverage_units=0;
}
int main(void) {
    static memory_t m, plain, covered;
    z80_t *cpu=create(&m);assert(cpu);
    assert(!cpu->coverage_enabled && !cpu->coverage_unique);
    z80_step(cpu);assert(!cpu->coverage_unique);
    cpu->coverage_enabled=true;
    for(unsigned pc=0;pc<65536;pc++) {
        cpu->pc=(uint16_t)pc;z80_step(cpu);assert(hit(cpu,pc));
    }
    assert(cpu->coverage_unique==65536 && cpu->coverage_units==65536);
    for(unsigned i=0;i<8192;i++)assert(cpu->coverage_bitmap[i]==255);
    z80_reset(cpu);assert(cpu->coverage_enabled && cpu->coverage_unique==65536);
    clear(cpu);
    /* No prefix operands get their own bits. HALT's following PC is unexecuted. */
    uint8_t code[]={0xdd,0x21,0x00,0x90,0xfd,0x21,0x00,0x91,0xcb,0x00,
        0xed,0x44,0xdd,0xcb,0,0x46,0xfd,0xcb,0,0x46,0x76,0};
    memcpy(m.ram+0x8000,code,sizeof(code));cpu->pc=0x8000;
    unsigned starts[]={0x8000,0x8004,0x8008,0x800a,0x800c,0x8010,0x8014};
    for(unsigned i=0;i<7;i++){z80_step(cpu);assert(hit(cpu,starts[i]));}
    assert(cpu->coverage_unique==7 && cpu->coverage_units==7);
    for(unsigned i=0;i<20;i++)z80_step(cpu);
    assert(cpu->coverage_units==7 && !hit(cpu,0x8015));
    cpu->coverage_enabled=false;cpu->halted=false;cpu->pc=0x8100;z80_step(cpu);
    assert(cpu->coverage_units==7 && !hit(cpu,0x8100));
    z80_destroy(cpu);
    /* Real dispatch + bus equivalence of enabled/disabled batch and step paths.
       This also exercises block repeats and a self-modifying store. */
    uint8_t program[]={0x31,0,0xa0,0x21,0,0x90,0x11,0,0x91,0x01,4,0,
        0xed,0xb0,0x3e,0,0x32,0x14,0x80,0x3c,0x3c,0x76};
    for(unsigned batch=0;batch<2;batch++) {
        memset(&plain,0,sizeof(plain));memset(&covered,0,sizeof(covered));
        memcpy(plain.ram+0x8000,program,sizeof(program));
        plain.ram[0x9000]=11;plain.ram[0x9001]=22;covered=plain;
        z80_t *a=create(&plain), *b=create(&covered);assert(a&&b);
        a->pc=b->pc=0x8000;b->coverage_enabled=true;
        for(unsigned i=0;i<100;i++) {
            if(batch){z80_execute(a,100);z80_execute(b,100);}
            else {z80_step(a);z80_step(b);}
            assert(memcmp(a,b,offsetof(z80_t,mread_cb))==0);
            assert(memcmp(&plain,&covered,sizeof(plain))==0);
        }
        assert(b->coverage_unique==10 && hit(b,0x800c) && !hit(b,0x800d));
        assert(!a->coverage_unique);
        z80_destroy(a);z80_destroy(b);
    }
    puts("PC coverage: 65536 addresses, prefixes/HALT/reset, batch/step bus/register/clock equivalence passed");
    return 0;
}
