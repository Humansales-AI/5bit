# THE SCHEDULER, WRITTEN IN 5BIT.
# The dependency analysis and the greedy cycle-assignment run as a DEF'd 5bit
# program on the Machine — READ/LOADX/STOREX/IF/LOOP over the grid. No Python
# logic in the scheduling: Python only lays the op arrays on the grid and reads
# the assigned cycles back to emit the target ISA's format at the boundary.
#
# Grid layout (arrays as contiguous slot ranges, indexed via LOADX/STOREX):
#   N            : number of ops
#   DEST[i]      : dest slot written by op i        (base DEST_B)
#   A[i], B[i]   : source slots read by op i         (base A_B, B_B)
#   ENG[i]       : engine id                         (base ENG_B)
#   CYC[i]       : assigned cycle (output)           (base CYC_B)
#   READY[i]     : earliest cycle op i can run       (base RDY_B)
#
# Dependency rule (RAW): op i must run strictly after any earlier op j whose
# DEST equals A[i] or B[i]. That comparison — "does column DEST match my source"
# — is the grid's vertical-compare / divergence primitive.

from griddb_interp import (Machine, verb, num, region, program,
    CMD_LOADX, CMD_STOREX, CMD_READ, CMD_STORE, CMD_IF, CMD_LOOP, CMD_BREAK)
from griddb_ownership import OwnershipEncoder
from griddb_alloc import AllocGrid
from binary_grid_db import Token, Encoder
import shutil

PLUS,MINUS,MUL,EMIT = Token(10),Token(11),Token(12),Token(14)

# scalar slots
N=1; I=2; J=3; C=4; TA=5; TB=6; TC=7; MAXC=8; PROG=9
# array bases (each up to NMAX entries)
NMAX=4096
DEST_B=100
A_B   =DEST_B+NMAX
B_B   =A_B+NMAX
CYC_B =B_B+NMAX
RDY_B =CYC_B+NMAX

def build_dep_and_ready():
    # For each op i (0..N-1): READY[i] = 1 + max(CYC[j]) over earlier j that i
    # depends on. Since we process in program order and assign CYC greedily as
    # READY (one op per "cycle-slot" is a simplification; the real ASAP schedule),
    # this computes the ASAP cycle = length of longest dependency chain to i.
    #
    #   for i in 0..N-1:
    #     r = 0
    #     for j in 0..i-1:
    #       if DEST[j]==A[i] or DEST[j]==B[i]:
    #         if CYC[j]+1 > r: r = CYC[j]+1
    #     CYC[i] = r
    inner = region(
        # ta = DEST[j]
        num(DEST_B), verb(CMD_READ,J), [PLUS], verb(CMD_LOADX), verb(CMD_STORE,TA),
        # tb = A[i]
        num(A_B), verb(CMD_READ,I), [PLUS], verb(CMD_LOADX), verb(CMD_STORE,TB),
        # tc = B[i]
        num(B_B), verb(CMD_READ,I), [PLUS], verb(CMD_LOADX), verb(CMD_STORE,TC),
        # dep if ta==tb or ta==tc  -> compute (ta-tb)*(ta-tc); ==0 means a match
        # match_flag via three-way IF on (ta-tb)
        verb(CMD_READ,TA), verb(CMD_READ,TB), [MINUS], verb(CMD_IF),
            region(  # ta != tb (positive): check ta vs tc
                verb(CMD_READ,TA), verb(CMD_READ,TC), [MINUS], verb(CMD_IF),
                    region(),  # no match
                    region(*_update_r()),  # ta==tc match
                    region(),
            ),
            region(*_update_r()),  # ta==tb match (zero arm)
            region(  # negative: check tc
                verb(CMD_READ,TA), verb(CMD_READ,TC), [MINUS], verb(CMD_IF),
                    region(),
                    region(*_update_r()),
                    region(),
            ),
        # j++
        verb(CMD_READ,J), num(1), [PLUS], verb(CMD_STORE,J),
    )
    return inner

def _update_r():
    # r = max(r, CYC[j]+1)   (r is slot C)
    return [
        num(CYC_B), verb(CMD_READ,J), [PLUS], verb(CMD_LOADX), num(1), [PLUS], verb(CMD_STORE,TA),
        # if TA > C: C = TA
        verb(CMD_READ,TA), verb(CMD_READ,C), [MINUS], verb(CMD_IF),
            region(verb(CMD_READ,TA), verb(CMD_STORE,C)),  # TA>C
            region(),
            region(),
    ]

def build_scheduler_program():
    # for i in 0..N-1:
    #   C = 0; J = 0
    #   while J < i: (inner dep scan)
    #   CYC[i] = C; track MAXC
    outer = region(
        num(0), verb(CMD_STORE,C),
        num(0), verb(CMD_STORE,J),
        # inner loop: while J < I
        verb(CMD_LOOP), region(
            verb(CMD_READ,J), verb(CMD_READ,I), [MINUS], verb(CMD_IF),
                region(verb(CMD_BREAK)),   # J>=I done
                region(verb(CMD_BREAK)),   # J==I done
                build_dep_and_ready(),     # J<I: scan
        ),
        # CYC[i] = C
        verb(CMD_READ,C), num(CYC_B), verb(CMD_READ,I), [PLUS], verb(CMD_STOREX),
        # track max
        verb(CMD_READ,C), verb(CMD_READ,MAXC), [MINUS], verb(CMD_IF),
            region(verb(CMD_READ,C), verb(CMD_STORE,MAXC)),
            region(), region(),
        # i++
        verb(CMD_READ,I), num(1), [PLUS], verb(CMD_STORE,I),
    )
    return program(PROG,
        num(0), verb(CMD_STORE,I),
        num(0), verb(CMD_STORE,MAXC),
        verb(CMD_LOOP), region(
            verb(CMD_READ,I), verb(CMD_READ,N), [MINUS], verb(CMD_IF),
                region(verb(CMD_BREAK)),
                region(verb(CMD_BREAK)),
                outer,
        ),
    )

def schedule_5bit(ops):
    """ops: list of (dest, a, b). Returns CYC[i] per op — the ASAP cycle,
    computed BY A 5BIT PROGRAM. Python only lays arrays + reads results."""
    n=len(ops)
    shutil.rmtree('/tmp/sched5', ignore_errors=True)
    g=AllocGrid(data_dir='/tmp/sched5'); own=OwnershipEncoder()
    slots=[N,I,J,C,TA,TB,TC,MAXC]+list(range(DEST_B,DEST_B+n))+list(range(A_B,A_B+n))+\
          list(range(B_B,B_B+n))+list(range(CYC_B,CYC_B+n))
    for s in slots: own.grant_w(slot=s,holder=0)
    def wr(slot,v): own.write_with_grant(g,slot,0,[*Encoder.encode_integer(v),Token.RECORD])
    wr(N,n)
    for i,(d,a,b) in enumerate(ops):
        wr(DEST_B+i,d); wr(A_B+i,a); wr(B_B+i,b); wr(CYC_B+i,0)
    m=Machine(ownership=own,grid=g,holder=0,max_steps=50_000_000)
    m.load(PROG, build_scheduler_program())
    m.run(PROG)
    return [m._parse_ints(g.read(CYC_B+i).tokens)[0] for i in range(n)]

if __name__=="__main__":
    # tiny proof: a=b+c ; d=a+e ; f=g+h (independent) 
    # op0: dest=10 a=11 b=12   (no deps -> cycle 0)
    # op1: dest=13 a=10 b=14   (reads 10=op0.dest -> cycle 1)
    # op2: dest=15 a=16 b=17   (independent -> cycle 0)
    ops=[(10,11,12),(13,10,14),(15,16,17)]
    cyc=schedule_5bit(ops)
    print("5bit-computed ASAP cycles:", cyc, "(expect [0,1,0])")
    assert cyc==[0,1,0], cyc
    print("THE SCHEDULER RAN IN 5BIT. Dependency chain length computed on the grid.")
