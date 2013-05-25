//_________________________________________________________________________________________________
//|                                                                                      -- INFO --
//| Name        : LutMap.cc
//| Author(s)   : Niklas Een
//| Module      : LutMap
//| Description : 
//| 
//| (C) Copyright 2013, The Regents of the University of California
//|________________________________________________________________________________________________
//|                                                                                  -- COMMENTS --
//| 
//|________________________________________________________________________________________________

#include "Prelude.hh"
#include "LutMap.hh"
#include "ZZ_Gig.hh"
#include "ZZ/Generics/Sort.hh"


namespace ZZ {
using namespace std;


//mmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmm
// Cut representation:


#define Cut LutMap_Cut      // -- avoid linking problems


class Cut {                // -- this class represents 6-input cuts.
    void extendAbstr(gate_id g) { abstr |= 1u << (g & 31); }

    gate_id inputs[6];
    uint    sz;
public:
    uint    abstr;

    Cut(Tag_empty) : sz(0), abstr(0) {}
    Cut(GLit p)    : sz(1), abstr(0) { inputs[0] = p.id; extendAbstr(p.id); }
    Cut()          : sz(7)           {}

    uint   size()                const { return sz; }
    GLit   operator[](int index) const { return GLit(inputs[index]); }
    bool   null()                const { return uint(sz) > 6; }
    void   mkNull()                    { sz = 7; }

    void   push(GLit p) { if (!null()){ inputs[sz++] = p.id; extendAbstr(p.id); } }
};

#define Cut_NULL Cut()


template<> fts_macro void write_(Out& out, const Cut& v)
{
    if (v.null())
        FWrite(out) "<null>";
    else{
        out += '{';
        if (v.size() > 0){
            out += v[0];
            for (uint i = 1; i < v.size(); i++)
                out += ',', ' ', v[i];
        }
        out += '}';
    }
}


//- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -


// Check if cut 'c' is a subset of cut 'd'. Cuts must be sorted. FTB is ignored.
macro bool subsumes(const Cut& c, const Cut& d)
{
    assert_debug(!c.null());
    assert_debug(!d.null());

    if (d.size() < c.size())
        return false;

    if (c.abstr & ~d.abstr)
        return false;

    if (c.size() == d.size()){
        for (uint i = 0; i < c.size(); i++)
            if (c[i] != d[i])
                return false;
    }else{
        uint j = 0;
        for (uint i = 0; i < c.size(); i++){
            while (c[i] != d[j]){
                j++;
                if (j == d.size())
                    return false;
            }
        }
    }

    return true;
}


macro bool moreThanSixBits(uint a)
{
  #if defined(__GNUC__)
    return __builtin_popcount(a) > 6;
  #else
    a &= a - 1;
    a &= a - 1;
    a &= a - 1;
    a &= a - 1;
    a &= a - 1;
    a &= a - 1;
    return a;
  #endif
}


// PRE-CONDITION: Inputs of 'cut1' and 'cut2' are sorted.
// Output: A cut representing AND of 'cut1' and 'cut2' with signs 'inv1' and 'inv2' respectively; 
// or 'Cut_NULL' if more than 6 inputs would be required.
static
Cut combineCuts_And(const Cut& cut1, const Cut& cut2)
{
    if (moreThanSixBits(cut1.abstr | cut2.abstr))
        return Cut_NULL;

    Cut   result(empty_);
    uint  i = 0;
    uint  j = 0;
    if (cut1.size() == 0) goto FlushCut2;
    if (cut2.size() == 0) goto FlushCut1;
    for(;;){
        if (result.size() == 6) return Cut_NULL;
        if (cut1[i] < cut2[j]){
            result.push(cut1[i]), i++;
            if (i >= cut1.size()) goto FlushCut2;
        }else if (cut1[i] > cut2[j]){
            result.push(cut2[j]), j++;
            if (j >= cut2.size()) goto FlushCut1;
        }else{
            result.push(cut1[i]), i++, j++;
            if (i >= cut1.size()) goto FlushCut2;
            if (j >= cut2.size()) goto FlushCut1;
        }
    }

  FlushCut1:
    if (result.size() + cut1.size() - i > 6) return Cut_NULL;
    while (i < cut1.size())
        result.push(cut1[i]), i++;
    goto Done;

  FlushCut2:
    if (result.size() + cut2.size() - j > 6) return Cut_NULL;
    while (j < cut2.size())
        result.push(cut2[j]), j++;
    goto Done;

  Done:
    return result;
}


//mmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmm
// LutMap class:


class LutMap {
    typedef Pair<float,float> Cost;

    // Input:
    const Params_LutMap& P;
    Gig&                 N;

    // State:
    SlimAlloc<Cut>    mem;
    WMap<Array<Cut> > cutmap;
    WMap<float>       area_est;
    WMap<float>       fanout_est;
    WMap<float>       arrival;
    WMap<float>       depart;

    uint              round;
    uint64            cuts_enumerated;      // -- for statistics
    float             target_arrival;

    uint64            mapped_area;
    float             mapped_delay;

    // Internal methods:
    void  evaluateCuts(Wire w, Array<Cut> cuts);
    void  generateCuts_And(Wire w, Vec<Cut>& out);
    void  generateCuts(Wire w);
    void  updateFanoutEst(bool instantiate);
    void  run();

    // Temporaries:
    Vec<Cut>  tmp_cuts;
    Vec<Cost> tmp_costs;

public:

    LutMap(Gig& N, Params_LutMap P);
};


//mmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmm
// Helper functions:


macro Pair<Cut, Array<Cut> > getCuts(Wire w, const WMap<Array<Cut> >& cutmap)
{
    assert(!sign(w));
    if (w == gate_Const)
        return tuple(Cut(empty_), Array<Cut>(empty_));
    else
        return tuple(Cut(+w), cutmap[w]);
}


// Add 'cut' to 'out' performing subsumption tests in both diretcions. If cut is constant or
// trivial, FALSE is returned (abort the cut enumeration), otherwise TRUE.
static
bool applySubsumptionAndAddCut(const Cut& cut, Vec<Cut>& out)
{
    if (cut.size() <= 1){
        // Constant cut, buffer or inverter:
        out.clear();
        out.push(cut);
        return false;
    }

    // Test for subsumption (note that in presence of subsumption, the resulting cut set is no longer unique)
    for (uint k = 0; k < out.size(); k++){
        if (subsumes(out[k], cut)){
            // Cut is subsumed by existing cut; don't add anything:
            return true; }

        if (subsumes(cut, out[k])){
            // Cut subsumes at least one existing cut; need to remove them all:
            out[k] = cut;
            for (k++; k < out.size();){
                assert_debug(!subsumes(out[k], cut));
                if (subsumes(cut, out[k])){
                    out[k] = out.last();
                    out.pop();
                }else
                    k++;
            }
            return true;
        }
    }
    out.push(cut);  // (non-subsuming and non-subsumed cut)
    return true;
}


//mmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmm
// Cut evalutation:


macro uint cutCost(const Cut& cut) {
    return 1; }     // -- all LUTs are created equal...


// Returns '(estimate-area, arrival-time)' of the best cut.
void LutMap::evaluateCuts(Wire w, Array<Cut> cuts)
{
    assert(cuts.size() > 0);

    Vec<Cost>& costs = tmp_costs;   // -- pairs '(delay, area)'
    costs.setSize(cuts.size());

    for (uint i = 0; i < cuts.size(); i++){
        costs[i] = tuple(0.0f, 0.0f);
        for (uint j = 0; j < cuts[i].size(); j++){
            Wire w = cuts[i][j] + N;
            newMax(costs[i].fst, arrival[w]);
            costs[i].snd += area_est[w];
        }
        costs[i].snd += cutCost(cuts[i]);
    }

    sobSort(ordByFirst(sob(costs), sob(cuts)));

    if (round > 0){
        float req_time = target_arrival - (depart[w] + 1);
        uint n = 1;
        for (; n < cuts.size(); n++)
            if (costs[n].fst > req_time)
                break;
        cuts.shrinkTo(n);
        costs.shrinkTo(n);

        /**/for (uint i = 0; i < costs.size(); i++) swp(costs[i].fst, costs[i].snd);
        sobSort(ordByFirst(sob(costs), sob(cuts)));
        /**/for (uint i = 0; i < costs.size(); i++) swp(costs[i].fst, costs[i].snd);

        // <<== need to make sure previous best cut is kept!
    }

    assert(fanout_est[w] > 0);
    area_est(w) = costs[0].snd / fanout_est[w];
    arrival(w) = costs[0].fst + 1.0f;
}


//mmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmm
// Cut generation:


/*
delay optimal everywhere
globally delay optimal, use slack for area recovery

departure estimation for non-mapped nodes? worth being conservative by having 'pessimistic_departure' using unmapped cuts?
make sure to keep previous best choice as an option in each cut list!
*/


void LutMap::generateCuts_And(Wire w, Vec<Cut>& out)
{
    assert(w == gate_And);
    assert(out.size() == 0);

    Wire u = w[0], v = w[1];
    Array<Cut> cs, ds;
    Cut triv_u, triv_v;
    l_tuple(triv_u, cs) = getCuts(+u, cutmap);
    l_tuple(triv_v, ds) = getCuts(+v, cutmap);

    // Compute cross-product:
    for (int i = -1; i < (int)cs.size(); i++){
        const Cut& c = (i == -1) ? triv_u : cs[i];
        for (int j = -1; j < (int)ds.size(); j++){
            const Cut& d = (j == -1) ? triv_v : ds[j];

            Cut cut = combineCuts_And(c, d);
            if (!cut.null() && !applySubsumptionAndAddCut(cut, out))
                return;
        }
    }
}


void LutMap::generateCuts(Wire w)
{
    switch (w.type()){
    case gate_Const:    // -- constants should really have been propagated before mapping, but let's allow for them
    case gate_PI:
    case gate_FF:
        // Base case -- Global sources:
        cutmap(w) = Array<Cut>(empty_);     // -- only the trivial cut
        area_est(w) = 0;
        arrival(w) = 0;
        break;

    case gate_And:
        // Inductive case:
        if (!cutmap[w]){
            Vec<Cut>& cuts = tmp_cuts;
            cuts.clear();
            generateCuts_And(w, cuts);
            cuts_enumerated += cuts.size();
            evaluateCuts(w, cuts.slice());
            cuts.shrinkTo(P.cuts_per_node);
            cutmap(w) = Array_copy(cuts, mem);
        }else
            evaluateCuts(w, cutmap[w]);
        break;

    case gate_PO:
    case gate_Seq:
        /*skip for now*/
        break;

    default:
        ShoutLn "INTERNAL ERROR! Unhandled gate type: %_", w.type();
        assert(false);
    }
}


//mmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmm
// Fanout estimation:


// Updates: 
//
//   - depart
//   - fanout_est
//   - mapped_area
//   - mapped_delay
//
void LutMap::updateFanoutEst(bool instantiate)
{
    // Compute the fanout count for graph induced by mapping:
    WMap<uint> fanouts(N, 0);
    fanouts.reserve(N.size());

    mapped_area = 0;
    depart.clear();

    For_DownOrder(N, w){
        if (w == gate_And){
            if (fanouts[w] > 0){
                const Cut& cut = cutmap[w][0];
                mapped_area += cutCost(cut);

                for (uint i = 0; i < cut.size(); i++){
                    Wire v = cut[i] + N;
                    fanouts(v)++;
                    newMax(depart(v), depart[w] + 1.0f);
                }
            }

        }else if (w == gate_PO)
            fanouts(w[0])++;
    }

    mapped_delay = 0.0f;
    For_Gates(N, w)
        if (isCI(w))
            newMax(mapped_delay, depart[w]);

    if (!instantiate){
        // Blend new values with old:
        uint  r = round + 1.0f;
        float alpha = 1.0f - 1.0f / (float)(r*r*r*r + 1.0f);
        float beta  = 1.0f - alpha;

        For_Gates(N, w){
            if (w == gate_And){
                fanout_est(w) = alpha * max_(fanouts[w], 1u)
                              + beta  * fanout_est[w];
                              //+ beta  * fanout_count[w];
            }
        }

    }else{
        // Build LUT representation:

        // <<== later
    }
}


//mmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmm
// Main:


void LutMap::run()
{
    round = 0;

    area_est  .reserve(N.size());
    fanout_est.reserve(N.size());

    // Initialize fanout estimation (and zero area estimation):
    {
        Auto_Gob(N, FanoutCount);
        For_Gates(N, w){
            area_est  (w) = 0;
            fanout_est(w) = nFanouts(w);
        }
    }

    // Techmap:
    for (round = 0; round < P.n_rounds; round++){
        double T0 = cpuTime();
        cuts_enumerated = 0;
        generateCuts(N.True());     // -- constants are not part of up-order
        For_UpOrder(N, w)
            generateCuts(w);
        double T1 = cpuTime();

        bool instantiate = (round == P.n_rounds - 1);
        updateFanoutEst(instantiate);
        double T2 = cpuTime();

        if (round == 0)
            target_arrival = mapped_delay * P.delay_factor;

        if (!P.quiet){
            if (round == 0)
                WriteLn "cuts_enumerated=%,d", cuts_enumerated;
            WriteLn "round=%d   mapped_area=%,d   mapped_delay=%_   [enum: %t, blend: %t]", round, mapped_area, mapped_delay, T1-T0, T2-T1;
        }
    }
}


LutMap::LutMap(Gig& N_, Params_LutMap P_) :
    P(P_), N(N_)
{
    if (!N.isCanonical()){
        /**/WriteLn "Compacting... %_", info(N);
        N.compact();
        /**/WriteLn "Done... %_", info(N);
    }

    run();

    // Free memory:
    for (uint i = 0; i < cutmap.base().size(); i++)
        dispose(cutmap.base()[i], mem);
    mem.clear(false);

    area_est  .clear(true);
    fanout_est.clear(true);
}


// Wrapper function:
void lutMap(Gig& N, Params_LutMap P) {
    LutMap inst(N, P); }


//mmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmm
}