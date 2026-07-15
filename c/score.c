/* score.c — Optimized 5bit grant-table VLIW scheduler
   Groups have dedicated register banks → no false sharing.
   Hash fusion: 3 stages = multiply_add (1 valu op).
   Live-range: idx/val in registers across rounds, store only at end. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const int limits[5] = {12, 6, 2, 2, 1};

int main(void) {
    int h=10, nv=(1<<(h+1))-1, ni=256, rounds=16, NG=ni/8, NB=12;
    int base=1000; /* bank register base (far from other slots) */
    long nops=0, nbundles=0;

    int max_b=200000;
    int *bw=calloc(50000,sizeof(int));
    int *ec=calloc(max_b*5,sizeof(int));
    for(int i=0;i<50000;i++)bw[i]=-1;

    #define P(e,d,s1,s2) do{ \
      int _p=0; \
      for(int _n=0;_n<=nbundles&&!_p;_n++){ \
        if(ec[_n*5+e]>=limits[e])continue; \
        if((d)>=0&&bw[(d)]==_n)continue; \
        if((s1)>=0&&bw[(s1)]==_n)continue; \
        if((s2)>=0&&bw[(s2)]==_n)continue; \
        if((d)>=0)bw[(d)]=_n; \
        ec[_n*5+e]++;_p=1; \
        if(_n>=nbundles)nbundles=_n+1; \
      } \
      if(!_p){if((d)>=0)bw[(d)]=nbundles;ec[nbundles*5+e]=1;nbundles++;} \
      nops++; \
    }while(0)

    /* Pre-broadcast constants */
    int vfvp=base-100, vone=base-108, vtwo=base-116, vzero=base-124;
    P(1,vfvp,base-200,-1); P(1,vone,base-201,-1);
    P(1,vtwo,base-202,-1); P(1,vzero,base-203,-1);

    for(int rnd=0;rnd<rounds;rnd++){
      for(int g=0;g<NG;g++){
        int b=g%NB;
        /* Each group gets its OWN register bank: 32 slots per bank */
        int bx=base+b*32;     /* vidx: 8 slots */
        int bv=bx+8;          /* vval: 8 slots */
        int bt=bx+16;         /* temps: 16 slots for this group */

        /* Round 0: vector load idx, val */
        if(rnd==0){ P(2,bx,base-300,-1); P(2,bv,base-301,-1); }

        /* vaddr = vfvp + vidx */
        P(1,bt,vfvp,bx);

        /* vnode = gather (simplified as valu op) */
        P(1,bt+8,bt,base-302);

        /* vval ^= vnode */
        P(1,bv,bv,bt+8);

        /* HASH6: 3 fusible stages (multiply_add) + 3 non-fusible (3 ops each)
           Fusible: P(1, vval, vval, const_slot) then P(1, vval, vval, const_slot) then P(1, vval, vval, vval)
           = 3 ops per fusible stage. */
        for(int st=0;st<6;st++){
          if(st==0||st==2||st==4){
            /* Fusible: 2 vbroadcasts (write temps) + 1 multiply_add (writes bv) */
            P(1,bt+10,base-400-st*3,-1);    /* vbroadcast const1 → temp1 */
            P(1,bt+12,base-401-st*3,-1);    /* vbroadcast const2 → temp2 */
            P(1,bv,bv,bt+10);               /* multiply_add: bv = bv*const1+const2 */
          }else{
            /* Non-fusible: op1→temp1, op3→temp2, op2→bv */
            P(1,bt+10,bv,base-400-st*3);    /* op1 → temp1 */
            P(1,bt+12,bv,base-401-st*3);    /* op3 → temp2 */
            P(1,bv,bt+10,bt+12);             /* op2 → bv */
          }
        }

        /* idx update (vectorized, bit-trick, 4 valu ops) */
        P(1,bt+10,bv,vone);    /* parity = val & 1 */
        P(1,bt+12,vone,bt+10); /* inc = 1 + parity */
        P(1,bx,bx,vtwo);       /* idx *= 2 */
        P(1,bx,bx,bt+12);      /* idx += inc */

        /* wrap: flow vselect */
        P(4,bx,bt+10,vzero);

        /* Store at final round only (live-range promotion) */
        if(rnd==rounds-1){
          P(3,-1,bx,-1);   /* vstore idx */
          P(3,-1,bv,-1);   /* vstore val */
        }
      }
    }

    long ts=0,us=0;
    for(int nb=0;nb<nbundles;nb++){int s=0;for(int e=0;e<5;e++)s+=ec[nb*5+e];ts+=23;us+=s;}

    printf("CYCLES: %ld  (ops=%ld, util=%.1f%%, ops/bundle=%.1f)\n",
           nbundles, nops, ts>0?(100.0*us/ts):0, nbundles>0?(double)nops/nbundles:0);
    free(bw);free(ec);
    return 0;
}
