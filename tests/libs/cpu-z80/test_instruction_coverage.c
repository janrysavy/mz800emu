/* SPDX-License-Identifier: MIT. Disposable CPU fixtures, never campaign evidence. */
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include "z80.h"

typedef struct {
    uint16_t address; uint8_t value,kind,m1; int tstate;
} event;
typedef struct { uint8_t ram[65536];event events[64];unsigned count;uint8_t latch; } bus;
static void record(z80_t *cpu,bus *b,unsigned address,unsigned value,unsigned kind,int m1) {
    assert(b->count<64);event *e=&b->events[b->count++];
    e->address=(uint16_t)address;e->value=(uint8_t)value;e->kind=(uint8_t)kind;e->m1=(uint8_t)m1;e->tstate=cpu->op_tstate;
}
static uint8_t mr(z80_t *cpu,uint16_t addr,int m1,void *data) {
    bus *b=(bus *)data;record(cpu,b,addr,b->ram[addr],0,m1);b->latch=b->ram[addr];return b->ram[addr];
}
static void mw(z80_t *cpu,uint16_t addr,uint8_t value,void *data) {
    bus *b=(bus *)data;record(cpu,b,addr,value,1,0);b->ram[addr]=value;
}
static uint8_t pr(z80_t *cpu,uint16_t addr,void *data) {
    record(cpu,(bus *)data,addr,0x5a,2,0);return 0x5a;
}
static void pw(z80_t *cpu,uint16_t addr,uint8_t value,void *data) {
    record(cpu,(bus *)data,addr,value,3,0);
}
static unsigned cases=0;
static int fast_mode=0;

static void check(const uint8_t *code,unsigned length,uint8_t flags,uint16_t bc,int outcome,int clocks,int batch) {
    bus a={0},b={0};memcpy(a.ram+0x8000,code,length);a.ram[0x4000]=0x01;a.ram[0x4001]=0x80;
    a.ram[0x9000]=0x10;b=a;
    z80_t *plain=z80_create(mr,&a,mw,&a,pr,&a,pw,&a,NULL,NULL);
    z80_t *captured=z80_create(mr,&b,mw,&b,pr,&b,pw,&b,NULL,NULL);assert(plain && captured);
    z80_t *cpus[]={plain,captured};
    for(unsigned i=0;i<2;i++) {
        cpus[i]->pc=0x8000;cpus[i]->sp=0x4000;cpus[i]->af.w=(uint16_t)(0x4000|flags);
        cpus[i]->bc.w=bc;cpus[i]->hl.w=0x9000;cpus[i]->de.w=0x9100;cpus[i]->ix.w=0x9000;cpus[i]->iy.w=0x9000;
        cpus[i]->coverage_enabled=1;
#ifdef MZ800EMU_CFG_RAM_FASTPATH
        if (fast_mode) {
            bus *memory=i ? &b:&a;uint8_t *pages[16];
            for(unsigned n=0;n<16;n++) pages[n]=memory->ram+4096*n;
            z80_set_ram_fastpath(cpus[i],pages,pages,&memory->latch,true);
        }
#endif
    }
    captured->instruction_coverage=zic_create(256);assert(captured->instruction_coverage);
    captured->instruction_coverage->enabled=1;
    int c1=batch ? z80_execute(plain,1) : z80_step(plain);
    int c2=batch ? z80_execute(captured,1) : z80_step(captured);
    assert(c1==clocks && c2==clocks);
    /* All register/interrupt/clock fields before the callback block, exact
     * guest RAM and the complete existing memory/port callback sequence. */
    assert(!memcmp(plain,captured,offsetof(z80_t,mread_cb)));
    assert(!memcmp(&a,&b,sizeof(a)));
    assert(plain->coverage_units==captured->coverage_units && plain->coverage_unique==captured->coverage_unique);
    assert(!memcmp(plain->coverage_bitmap,captured->coverage_bitmap,sizeof(plain->coverage_bitmap)));
    zic_state *s=captured->instruction_coverage;
    assert(zic_complete(s) && s->units==1 && s->unique==1 && s->rows[0].key.outcome==outcome);
    assert(s->rows[0].key.pc==0x8000 && s->rows[0].key.next_pc==captured->pc);
    assert(s->rows[0].key.length==length && !memcmp(s->rows[0].key.bytes,code,length));
    assert(s->rows[0].key.m1_mask==((code[0]==0xdd || code[0]==0xfd || code[0]==0xcb || code[0]==0xed) ? 3:1));
    z80_destroy(plain);z80_destroy(captured);cases++;
}

static void lifetime(void) {
    bus b={0};const uint8_t code[]={0xdd,0,0x76};memcpy(b.ram+0x8000,code,sizeof(code));
    z80_t *cpu=z80_create(mr,&b,mw,&b,pr,&b,pw,&b,NULL,NULL);assert(cpu);cpu->pc=0x8000;
    cpu->coverage_enabled=1;cpu->instruction_coverage=zic_create(256);assert(cpu->instruction_coverage);
    zic_state *s=cpu->instruction_coverage;s->enabled=1;
    assert(z80_step(cpu)==4 && cpu->pc==0x8001 && s->units==1 && zic_complete(s));
    assert(s->rows[0].key.flags==ZIC_FRAGMENT && s->rows[0].key.length==2);
    assert(z80_step(cpu)==4 && z80_step(cpu)==4 && cpu->halted && s->units==3);
    assert(z80_step(cpu)==4 && s->units==3 && cpu->coverage_units==3);
    z80_reset(cpu);assert(cpu->instruction_coverage==s && s->units==3 && s->enabled);
    assert(z80_step(cpu)==4 && s->units==4 && zic_complete(s));
    zic_reset(s);cpu->pc=0x8000;b.ram[0x8000]=0x3e;b.ram[0x8001]=0x42;
    assert(z80_step(cpu)==7);cpu->pc=0x8000;b.ram[0x8001]=0x43;
    assert(z80_step(cpu)==7 && s->unique==2 && zic_complete(s));
    assert(s->rows[0].key.bytes[1]==0x42 && s->rows[1].key.bytes[1]==0x43);
    z80_destroy(cpu);
}

int main(void) {
    const uint8_t flagbits[]={0x40,0x40,0x01,0x01,0x04,0x04,0x80,0x80};
    int fast_modes=1;
#ifdef MZ800EMU_CFG_RAM_FASTPATH
    fast_modes=2;
#endif
    for(fast_mode=0;fast_mode<fast_modes;fast_mode++) for(int batch=0;batch<2;batch++) {
        for(unsigned cc=0;cc<8;cc++) for(int taken=0;taken<2;taken++) {
            uint8_t f=(uint8_t)(((cc&1) ? taken : !taken) ? flagbits[cc]:0);
            uint8_t ret[]={(uint8_t)(0xc0+8*cc)};
            uint8_t jp[]={(uint8_t)(0xc2+8*cc),0x03,0x80};
            uint8_t call[]={(uint8_t)(0xc4+8*cc),0x03,0x80};
            check(ret,1,f,0,taken,taken ? 11:5,batch);
            check(jp,3,f,0,taken,10,batch);
            check(call,3,f,0,taken,taken ? 17:10,batch);
            if(cc<4) {
                uint8_t jr[]={(uint8_t)(0x20+8*cc),0};
                check(jr,2,f,0,taken,taken ? 12:7,batch);
            }
        }
        for(int taken=0;taken<2;taken++) {
            const uint8_t code[]={0x10,0};check(code,2,0,taken ? 0x0200:0x0100,taken,taken ? 13:8,batch);
            const uint8_t blocks[]={0xb0,0xb1,0xb2,0xb3,0xb8,0xb9,0xba,0xbb};
            for(unsigned i=0;i<sizeof(blocks);i++) {
                uint8_t code[]={0xed,blocks[i]};
                uint16_t bc=(blocks[i]&2) ? (taken ? 0x0200:0x0100) : (taken ? 2:1);
                check(code,2,0,bc,taken,taken ? 21:16,batch);
            }
        }
        const uint8_t dd[]={0xdd,0x21,0x34,0x12};check(dd,4,0,0,-1,14,batch);
        const uint8_t fd[]={0xfd,0x21,0x34,0x12};check(fd,4,0,0,-1,14,batch);
        const uint8_t cb[]={0xcb,0x06};check(cb,2,0,0,-1,15,batch);
        const uint8_t ddcb[]={0xdd,0xcb,0x01,0x46};check(ddcb,4,0,0,-1,20,batch);
        const uint8_t neg[]={0xed,0x44};check(neg,2,0,0,-1,8,batch);
    }
    lifetime();
    printf("PASS: %u enabled/disabled step/batch cases; exact clocks, CPU, RAM, callback events and PC maps\n",cases);
    return 0;
}
