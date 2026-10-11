/* SPDX-License-Identifier: MIT. Synthetic collector controls, not gameplay. */
#include <assert.h>
#include <stdio.h>
#include "instruction_coverage.h"

static void immediate(zic_state *s,uint8_t value) {
    zic_begin(s,0x8000);
    assert(zic_fetch(s,0x8000,0x3e,1)==0x3e);
    zic_fetch(s,0x8001,value,0);
    zic_finish(s,0x8002);
}

int main(void) {
    zic_state *s=zic_create(4);assert(s);s->enabled=1;
    immediate(s,0x42); immediate(s,0x42); immediate(s,0x43);
    assert(s->units==3 && s->unique==2 && s->rows[0].count==2 && zic_complete(s));
    assert(s->rows[0].first_dispatch==1 && s->rows[0].last_dispatch==2);
    assert(s->rows[0].key.m1_mask==1 && s->rows[0].key.bytes[1]==0x42);

    /* JR NZ,+0 has identical destinations for false and true decisions. */
    zic_reset(s);
    for (int taken=0;taken<=1;taken++) {
        zic_begin(s,0x8000);zic_fetch(s,0x8000,0x20,1);zic_fetch(s,0x8001,0,0);
        assert(zic_decision(s,taken)==taken);zic_finish(s,0x8002);
    }
    assert(s->unique==2 && s->rows[0].key.outcome==0 && s->rows[1].key.outcome==1);
    assert(zic_complete(s));

    /* A missing actual decision is an explicit failed capture. */
    zic_reset(s);zic_begin(s,0x8000);zic_fetch(s,0x8000,0xc0,1);zic_finish(s,0x8001);
    assert(s->decision_failures==1 && s->dropped==1 && !zic_complete(s));
    zic_reset(s);zic_begin(s,0x8000);zic_fetch(s,0x8001,0,1);zic_finish(s,0x8002);
    assert(s->fetch_failures==1 && !zic_complete(s));

    zic_reset(s);zic_begin(s,0xfff8);
    for (unsigned i=0;i<ZIC_MAX_FETCH+1;i++) zic_fetch(s,(uint16_t)(0xfff8+i),0,1);
    zic_finish(s,9);assert(s->fetch_failures==1 && !zic_complete(s));

    /* A genuine DD fragment is retained explicitly, not decoded as a JR. */
    zic_reset(s);zic_begin(s,0x8000);
    zic_fetch(s,0x8000,0xdd,1);zic_fetch(s,0x8001,0x20,1);
    zic_flag(s,ZIC_FRAGMENT);zic_finish(s,0x8001);
    assert(zic_complete(s) && s->rows[0].key.flags==ZIC_FRAGMENT);

    zic_reset(s);s->units=((uint64_t)1<<40);immediate(s,9);
    assert(s->units==(((uint64_t)1<<40)+1) && s->rows[0].first_dispatch==s->units);
    s->enabled=0;immediate(s,10);assert(s->unique==1);
    zic_destroy(s);

    /* Exhaust capacity, then verify already-known tuples can still count. */
    s=zic_create(2);assert(s);s->enabled=1;
    immediate(s,1);immediate(s,2);immediate(s,3);immediate(s,1);
    assert(s->unique==2 && s->units==4 && s->capacity_failures==1 && s->dropped==1);
    assert(s->rows[0].count==2 && !zic_complete(s));zic_destroy(s);

    /* Force an actual hash bucket collision; exact keys must stay separate. */
    s=zic_create(4);assert(s);s->enabled=1;
    unsigned found=0,first=0,second=0;
    for (unsigned a=0;a<256 && !found;a++) for(unsigned b=a+1;b<256;b++) {
        zic_key x={0},y={0};x.pc=y.pc=0x8000;x.next_pc=y.next_pc=0x8002;
        x.length=y.length=2;x.bytes[0]=y.bytes[0]=0x3e;x.outcome=y.outcome=-1;
        x.m1_mask=y.m1_mask=1;x.bytes[1]=(uint8_t)a;y.bytes[1]=(uint8_t)b;
        if ((zic_hash(&x)&7)==(zic_hash(&y)&7)) {first=a;second=b;found=1;break;}
    }
    assert(found);immediate(s,(uint8_t)first);immediate(s,(uint8_t)second);
    assert(s->unique==2 && zic_complete(s));zic_destroy(s);
    assert(!zic_create(3) && !zic_create(0));
    puts("PASS: exact tuples, outcomes, failures, counters, fragments and collisions");
    return 0;
}
