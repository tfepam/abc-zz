//_________________________________________________________________________________________________
//|                                                                                      -- INFO --
//| Name        : IncPdr.cc
//| Author(s)   : Niklas Een
//| Module      : Gip
//| Description :
//|
//| (C) Copyright 2013, The Regents of the University of California
//|________________________________________________________________________________________________
//|                                                                                  -- COMMENTS --
//|
//|________________________________________________________________________________________________

#include "Prelude.hh"


namespace ZZ {
using namespace std;


//mmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmm
// Proof Obligation:


static const uint frame_INF  = UINT_MAX;
static const uint frame_NULL = UINT_MAX - 1;
static const uint frame_CEX  = UINT_MAX - 2;


// A proof obligation (Pobl) is a timed cube 'tcube = (cube, frame)' which has to be blocked,
// together with a priority 'prio'. Pobls are handled from the smallest frame number to the largest,
// and for ties, from the smallest priority to the the largest. Each PO stores a reference-counted
// pointer to the 'next' Pobl that gave rise to it. If property fails, following this chain
// produces a counterexample.
//
struct Pobl_Data {
    Cube    cube;
    uint    frame;
    uint    prio;

    RefC<Pobl_Data> next;
    uint            refC;
};


struct Pobl : RefC<Pobl_Data> {
    Pobl() : RefC<Pobl_Data>() {}
        // -- create null object

    Pobl(Cube cube, uint frame, uint prio, Pobl next = Pobl()) :
        RefC<Pobl_Data>(empty_)
    {
        (*this)->cube  = cube;
        (*this)->frame = frame;
        (*this)->prio  = prio;
        (*this)->next  = next;
    }

    Pobl(const RefC<Pobl_Data> p) : RefC<Pobl_Data>(p) {}
        // -- downcast from parent to child
};


macro bool operator<(const Pobl& x, const Pobl& y) {
    assert(x); assert(y);
    return x->tcube.frame < y->tcube.frame || (x->tcube.frame == y->tcube.frame && x->prio < y->prio); }


//mmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmm
// Frame Cube -- 'FCube':


struct FCube {
    Cube    unreach;
    Cube    trigger;
    uint    idx;
    FCube(Cube unreach_ = Cube_NULL, Cube trigger_ = Cube_NULL) : unreach(unreach_), trigger(trigger_) {}
};


//mmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmm
// Class 'IncPdr':


class IncPdr {
    MiniSat2 S;
    MiniSat2 SI;
    MiniSat2 SB;

    Vec<lbool> model;   // -- last model from a SAT call

    uint prioC;         // -- priority of proof-obligation (lower goes first)

public:
  //________________________________________
  //  Public state: [read-only]

    Gig&                N;
    Out*                out;

    Vec<Vec<FCube> >    F;
    Vec<FCube>          F_inf;
    KeyHeap<ProofObl>   Q;

    Pobl                cex;    // -- set when 'solve()' returns 'ip_Cex'

  //________________________________________
  //  Public methods:

    IncPdr(Gig& N_, Out* out_ = NULL);

    uint solve(Cube target, uint frame, double effort = DBL_MAX, bool clear_Q = true);
        // -- returns the last frame in which 'target' was proved unreachable (most likely 'frame'
        // itself). 'frame_INF' means 'target' is forever unreachable; 'frame_CEX' means
        // counterexample was found (stored in class global 'cex'); 'frame_NULL' means effort
        // level was exceeded (where a positive effort means '#conflicts'; negative CPU-time).

    uint subsumed(Cube target, uint frame);
        // -- returns the latest time-frame where 'target.cube' is syntactically subsumed or
        // 'frame_NULL' if not subsumed at the frame given by the 'target.frame'.

    void extractCex(Cex& out_cex);
};

frame_NULL -- timeout
frame_CEX -- CEX found


//mmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmm
// Implementation:


IncPdr::IncPdr(Gig& N_, const IncPdr_Params& P_, Out* out_ = NULL) :
    prioC(UINT_MAX),
    N(N_),
    P(P_),
    out(out_)
{
    if (!out) out = &std_out;
}


uint IncPdr::solve(Cube target, uint frame, double effort = DBL_MAX, bool clear_Q)
{
    if (clear_Q){
        Q.clear();
        prioC = UINT_MAX; }
    Q.add(Pobl(target, frame, prioC--));

    for(;;){
        Pobl po = Q.pop();

        if (po->frame == 0){    // <<= change later when self-abstracting is in place
            cex = po;
            return frame_CEX; }

        uint d = subsumed(po->cube, po->frame);
        if (d == frame_NULL){
            Cube s;
            l_tuple(s, d) = solveRel(po->cube, po->frame);
            if (d == frame_NULL){
                // SAT -- extract model and create new proof-obligation:
                addPobl(s, po->cube, po->frame);

            }else{
                // UNSAT -- generalize cube, check triggers, check termination:
                l_tuple(s, d) = generalize(s, d);
                addCube(s, d);
            }

        }else if (!po->next){
            // Proof-obligation was target and was proved:
            assert(po->cube == target && d >= frame);
            return d;
        }

        // Re-insert proof-obligation at the right time-frame:
        if (d != frame_INF){
            if (d != frame_NULL)
                po->frame = d + 1;
            Q.add(po);
        }

#if 0
        // Check effort:
        if (<resources exhausted>)
            return frame_NULL;
#endif
    }
}


bool IncPdr::subsumed(Cube target, uint frame)
{

}


bool IncPdr::isInit(Cube target, Cube* sub_target = NULL)
{
}

// Add cube, remove subsumed cube and push triggered clauses forward
void IncPdr::addCube(Cube target, uint frame)
{
}


// 'pred' is the predecessor min-term extracted from the SAT-solver model which in one
// transition hits the cube 'target' in frame 'frame'.
void IncPdr::addPobl(Cube pred, Cube target, uint frame)
{
    // weaken lhs cube (justify/weaken by sim)
    // store
}


// Assuming 'cube' is unreachable in frame 'frame', return the latest frame where it is 
// unreachable. If SAT solver gives information for free, a subset of 'cube' may be returned.
// This information may optionally be seeded by using 'sub_cube' (the default null-value 
// will be replace by 'cube' itself).
Pair<Cube, uint> pushFwd(Cube cube, uint frame, Cube sub_cube = Cube_NULL)
{
    // setup solve-rel problem without inductive assumptiond and without extracting model 
    // (and no special handling of initial state/frame 0.
}


// See if cube 'target' can be reached in frame 'frame' using what we know about
// previous time frame (+ inductive assumption, using 'target' itself). 
// If SAT: returns '(predecessor-minterm, frame_NULL)'
// If UNSAT: returns '(subset-of-target, last-frame-target-is-unreach)'
Pair<Cube, uint> solveRel(Cube target, uint frame)
{
    if (frame == 0){
        Cube s;
        return isInit(target, &s) ? tuple(s, frame_NULL) : pushFwd(s, 0);

    }else{
    // Inductive assumption:
    // Assume 's' at state outputs:
    // Solve:
        // Figure out subset of 's' enough for UNSAT; do pushFwd.
        // ...or return model
    }

    // add/recycle temporary activation literal
}


Pair<Cube, uint> generalize(Cube cube, uint frame)
{
}


//mmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmm


unreach kuber
satisfierbara kuber/fulla modeller (triggers)

när ny kub härleds vill vi hitta alla triggers (eller göra det level by level?)
subsumering bland pobl?


optionally clear Q
while (Q.size) > 0 && time < timeout){
    pobl = Q.pop()

    if subsumed, move it to later timeframe (unless infinity) PLUS check if pobl was original pobl => DONE
    else{
        if (isInitital())
            assert(frame == 0 || self_abstracting)
            return CEX
        }

        solveRel();
        if (blocked by prev. time-frame){
            generalize cube (first depth, then size)
            insert cube and removed subsumed cubes
            check triggers (push cubes, recheck triggers...)
            move pobl forward
        }else{
            enqueue new pobl
        }
    }
}


Vec<Set<Unr+Trig> >
Vec<Queue<Pobl> >


//mmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmm


void solve(Cube c, uint frame, bool clear_Q)
{
    if (clear_Q) Q.clear();

    Q.add(c, frame);
    for(;;){

    }
}


//mmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmm
}
