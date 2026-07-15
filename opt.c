#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static const int L[5]={12,6,2,2,1};
int main(void){
  int NG=256/8,R=16,NB=32,ba=5000,bb=7000; long nops=0,nb=0;
  int*bw=calloc(99999,4),*ec=calloc(999999*5,4),*lr=calloc(99999,4);
  for(int i=0;i<99999;i++)bw[i]=lr[i]=-1;
#define P(e,d,s1,s2) do{int p=0,m=0;if((s1)>=0&&lr[(s1)]>=0&&lr[(s1)]+1>m)m=lr[(s1)]+1;if((s2)>=0&&lr[(s2)]>=0&&lr[(s2)]+1>m)m=lr[(s2)]+1;for(int n=m;n<=nb&&!p;n++){if(ec[n*5+e]>=L[e])continue;if((d)>=0&&bw[(d)]==n)continue;if((s1)>=0&&lr[(s1)]>=0&&bw[(s1)]==n)continue;if((s2)>=0&&lr[(s2)]>=0&&bw[(s2)]==n)continue;if((d)>=0)bw[(d)]=n;ec[n*5+e]++;p=1;if(n>=nb)nb=n+1;if((d)>=0)lr[(d)]=n;}if(!p){int n=nb>m?nb:m;if((d)>=0)bw[(d)]=n;ec[n*5+e]=1;if(n>=nb)nb=n+1;if((d)>=0)lr[(d)]=n;}nops++;}while(0)
  /* Init: vbroadcast constants */
  int vf=3000,vo=3008,vt=3016,vz=3024;
  P(1,vf,-1,-1);P(1,vo,-1,-1);P(1,vt,-1,-1);P(1,vz,-1,-1);
  int hc[12];for(int i=0;i<12;i++){hc[i]=3000-300-i*8;P(1,hc[i],-1,-1);}
  /* Top 6 levels = 63 nodes. Rounds 0-4 and 10-14 stay resident (10 of 16). */
  int res[16]={1,1,1,1,1,0,0,0,0,0,1,1,1,1,1,0};
  for(int g=0;g<NG;g++){int b=g%NB;
    for(int r=0;r<R;r++){int base=(r&1)?bb:ba,bx=base+b*32,bv=bx+8,bt=bx+16;
      /* Loads: idx, val (load engine=2) */
      P(2,bx,-1,-1);P(2,bv,-1,-1);
      /* vaddr = fvp + idx (valu=1) */
      P(1,bt,vf,bx);
      /* vnode: register-resident (flow=4) or gather (load=2 per lane) */
      if(res[r])P(4,bt+8,bt,-1);else for(int l=0;l<8;l++)P(2,bt+8+l,bt,-1);
      /* XOR val ^ node (valu=1) */
      P(1,bv,bv,bt+8);
      /* Hash: fusible=valu, non-fusible op1+op3=alu(0), op2=valu(1) */
      for(int s=0;s<6;s++){int t1=bt+20+s*2,t2=bt+22+s*2;
        if(s==0||s==2||s==4)P(1,bv,bv,hc[s/2*2]);
        else{P(0,t1,bv,hc[(s/2)*2]);P(0,t2,bv,hc[(s/2)*2+1]);P(1,bv,t1,t2);}}
      /* idx: parity+inc on ALU(0), multiply_add on VALU(1), wrap on VALU(1) */
      P(0,bt+34,bv,vo);P(0,bt+42,vo,bt+34);P(1,bx,bx,bt+42);if(r>=10)P(1,bx,bx,vo);
      /* Stores (store=3) */
      P(3,-1,bx,-1);P(3,-1,bv,-1);}}
  printf("%ld\n",nb);free(bw);free(ec);free(lr);return 0;
}
