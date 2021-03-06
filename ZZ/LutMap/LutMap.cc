//_________________________________________________________________________________________________
//|                                                                                      -- INFO --
//| Name        : LutMap.cc
//| Author(s)   : Niklas Een
//| Module      : LutMap
//| Description :
//|
//| (C) Copyright 2010-2014, The Regents of the University of California
//|________________________________________________________________________________________________
//|                                                                                  -- COMMENTS --
//|
//|________________________________________________________________________________________________

#include "Prelude.hh"
#include "LutMap.hh"
#include "ZZ_Gig.hh"
#include "ZZ_BFunc.hh"
#include "ZZ_Npn4.hh"
#include "ZZ/Generics/Sort.hh"
#include "ZZ/Generics/Heap.hh"
#include "Cut.hh"
#include "Unmap.hh"
#include "PostProcess.hh"
//**/#include "ZZ_Dsd.hh"

#define ELA_GLOBAL_LIM 500      // -- if more nodes than this is dereferenced, MFFC is too big to consider
#define ELA_TIMING

namespace ZZ {
using namespace std;


//mmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmm
// Helpers:


// For debugging
void checkFmuxes(Gig& N)
{
    // Max two levels of MUXes:
    For_Gates(N, w){
        if (w == gate_Mux){
            if (w[1] == gate_Mux){
                /**/if (!(w[1][1] != gate_Mux)) putchar('*'), fflush(stdout);
                assert(w[1][1] != gate_Mux);
                /**/if (!(w[1][2] != gate_Mux)) putchar('*'), fflush(stdout);
                assert(w[1][2] != gate_Mux);
            }

            if (w[2] == gate_Mux){
                /**/if (!(w[2][1] != gate_Mux)) putchar('*'), fflush(stdout);
                assert(w[2][1] != gate_Mux);
                /**/if (!(w[2][2] != gate_Mux)) putchar('*'), fflush(stdout);
                assert(w[2][2] != gate_Mux);
            }
        }
    }

    {
        WMap<uint> mux_fanouts(0);

        uint total = 0;
        uint inv_failC = 0;
        uint out_failC = 0;
        uint inp_failC = 0;
        For_Gates(N, w){
            if (w != gate_Mux) continue;

            for (uint i = 1; i <= 2; i++){
                if (w[i] == gate_Mux){
                    mux_fanouts(w[i])++;
                    if (mux_fanouts[w[i]] >= 2)
                        out_failC++;
                }
            }
        }

        For_Gates(N, w){
            if (w != gate_Mux) continue;

            total++;
            if ((w[1].sign && mux_fanouts[w[1]] > 1) || (w[2].sign && mux_fanouts[w[2]] > 1))
                inv_failC++;
        }


        For_Gates(N, w){
            if (w != gate_Mux) continue;

            if (w[1] != gate_Lut6 && w[1] != gate_Mux) inp_failC++;
            if (w[2] != gate_Lut6 && w[2] != gate_Mux) inp_failC++;
        }

        if (inv_failC > 0)
            WriteLn "WARNING! Non-trivial MUX inverter violations: %_ / %_  (= %.2f %%)", inv_failC, total, double(inv_failC) / total * 100;
        if (out_failC > 0)
            WriteLn "WARNING! Actual MUX fanout violations: %_ / %_  (= %.2f %%)", out_failC, total, double(out_failC) / total * 100;
        if (inp_failC > 0)
            WriteLn "WARNING! Non-LUT feeding MUX: %_ / %_  (= %.2f %%)", inp_failC, total, double(inp_failC) / total * 100;
    }
}


macro bool isLogic(Wire w) {
    return w == gate_And || w == gate_Lut4;
}


//mmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmm
// LutMap class:


#define Cut LutMap_Cut
#define Cut_NULL Cut()


struct LutMap_Cost {
    uint    idx;
    float   delay;
    float   area;
    uint    cut_size;
    float   avg_fanout;
};


class LutMap {
    typedef LutMap_Cost Cost;

    // Input:
    const Params_LutMap& P;
    Gig&                 N;
    WMapX<GLit>*         remap;

    // State:
    SlimAlloc<Cut>    mem;
    WMap<Array<Cut> > cutmap;
    WMap<Cut>         winner;
    WMap<float>       area_est;
    WMap<float>       fanout_est;
    WMap<float>       arrival;
    WMap<float>       depart;
    WMap<uchar>       active;

    uint              round;
    uint64            cuts_enumerated;      // -- for statistics
    float             target_arrival;
    float             best_arrival;

    uint64            mapped_area;
    float             mapped_delay;

    // Internal methods:
    void  prioritizeCuts(Wire w, Array<Cut> cuts);
    void  reprioritizeCuts(Wire w, Array<Cut> cuts);
    void  generateCuts_LogicGate(Wire w, Vec<Cut>& out);
    void  generateCuts(Wire w);
    void  updateFanoutEst(bool instantiate);
    void  run();

    uint  derefCut(const Gig& N, const Cut& cut, WMap<uint>& fanouts);
    uint  refCut  (const Gig& N, const Cut& cut, WMap<uint>& fanouts);
    uint  tryCut  (const Gig& N, const Cut& cut, WMap<uint>& fanouts);
    void  exactLocalArea(WMap<uint>& fanouts);


    // Temporaries:
    Vec<Cut>     tmp_cuts;
    Vec<Cost>    tmp_costs;
    Vec<uint>    tmp_where;
    Vec<uint>    tmp_list;
    WZet         in_memo;
    WMap<uint64> memo;

    bool probeRound() const { return P.map_for_delay && round == 0; }
    bool delayRound() const { return P.map_for_delay ? round == 1 : round == 0; }

public:

    LutMap(Gig& N, Params_LutMap P, WMapX<GLit>* remap);
};


//mmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmm
// Helper functions:


static
uint64 computeFtb_(Wire w, const Cut& cut, WZet& in_memo, WMap<uint64>& memo)
{
    uint64 imask = (w.sign ? 0xFFFFFFFFFFFFFFFFull : 0ull);

    if (w.id == gid_True)
        return ~imask;

    if (in_memo.has(w))
        return memo[w] ^ imask;

    uint64 ret;
    for (uint i = 0; i < cut.size(); i++)
        if (w.id == cut[i]){
            ret = ftb6_proj[0][i];
            goto Found; }
    /*else*/
    {
        if (w == gate_And)
            ret = computeFtb_(w[0], cut, in_memo, memo) & computeFtb_(w[1], cut, in_memo, memo);
        else if (w == gate_Lut4){
            uint64 ftb[4];
            for (uint i = 0; i < 4; i++)
                ftb[i] = w[i] ? computeFtb_(w[i], cut, in_memo, memo) : 0ull;

            ret = 0;
            for (uint64 m = 1; m != 0; m <<= 1){
                uint b = uint(bool(ftb[0] & m)) | (uint(bool(ftb[1] & m)) << 1) | (uint(bool(ftb[2] & m)) << 2) | (uint(bool(ftb[3] & m)) << 3);
                if (w.arg() & (1 << b))
                    ret |= m;
            }

        }else
            assert(false);
    }Found:;

    memo(w) = ret;
    in_memo.add(w);

    return ret ^ imask;
}


static
uint64 computeFtb(Wire w, const Cut& cut, WZet& in_memo, WMap<uint64>& memo)
{
    uint64 ret = computeFtb_(w, cut, in_memo, memo);
    in_memo.clear();
    return ret;
}


//mmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmm
// Cut evalutation:


struct Delay_lt {
    bool operator()(const LutMap_Cost& x, const LutMap_Cost& y) const {
        if (x.delay < y.delay) return true;
        if (x.delay > y.delay) return false;
        /**/if (x.cut_size < y.cut_size) return true;
        /**/if (x.cut_size > y.cut_size) return false;
        if (x.area < y.area) return true;
        if (x.area > y.area) return false;
        if (x.avg_fanout > y.avg_fanout) return true;
        if (x.avg_fanout < y.avg_fanout) return false;
        return false;
//        return x.cut_size < y.cut_size;
    }
};


struct Area_lt {
    bool operator()(const LutMap_Cost& x, const LutMap_Cost& y) const {
        if (x.area < y.area) return true;
        if (x.area > y.area) return false;
        /**/if (x.cut_size < y.cut_size) return true;
        /**/if (x.cut_size > y.cut_size) return false;
        if (x.delay < y.delay) return true;
        if (x.delay > y.delay) return false;
        if (x.avg_fanout > y.avg_fanout) return true;
        if (x.avg_fanout < y.avg_fanout) return false;
        return false;
//        return x.cut_size < y.cut_size;
    }
};


void LutMap::prioritizeCuts(Wire w, Array<Cut> cuts)
{
    assert(cuts.size() > 0);
    assert(fanout_est[w] > 0);

    // Setup cost vector:
    Vec<Cost>& costs = tmp_costs;
    costs.setSize(cuts.size());

    for (uint i = 0; i < cuts.size(); i++){
        costs[i].idx = i;
        costs[i].delay = 0.0f;
        costs[i].area  = 0.0f;
        costs[i].cut_size = cuts[i].size();
        costs[i].avg_fanout = 0.0f;

        for (uint j = 0; j < cuts[i].size(); j++){
            Wire w = cuts[i][j] + N;
            newMax(costs[i].delay, arrival[w]);
            costs[i].area += area_est[w];
            costs[i].avg_fanout += fanout_est[w];
        }
        if (cuts[i].mux_depth == 0){
            // Normal LUT:
            costs[i].area += P.lut_cost[cuts[i].size()];
            costs[i].delay += 1.0f;
        }else{
            costs[i].area += P.mux_cost * cuts[i].mux_depth;
            /**/costs[i].area *= fanout_est[w];
            // <<==FT do something about delay for selector here?
        }
        costs[i].avg_fanout /= cuts[i].size();
    }

    // Compute order:
    sobSort(sob(costs, Delay_lt()));
    float req_time = 0;
    if (!probeRound() && !delayRound()){
        assert(depart[w] != FLT_MAX);
        req_time = target_arrival - (depart[w] + 1);

        uint j = 0;
        for (uint i = 0; i < costs.size(); i++)
            if (costs[i].delay <= req_time)
                swp(costs[i], costs[j++]);

        Array<Cost> pre = costs.slice(0, j);
        sobSort(sob(pre, Area_lt()));

        uint n_delay_cuts = uint(P.cuts_per_node * 0.3) + 1;
        Array<Cost> suf = costs.slice(min_(j, costs.size() - n_delay_cuts));
        sobSort(sob(suf, Delay_lt()));
    }

    // Implement order:
    Vec<uint>& where = tmp_where;
    Vec<uint>& list  = tmp_list;
    where.setSize(cuts.size());
    list .setSize(cuts.size());
    for (uint i = 0; i < cuts.size(); i++)
        where[i] = list[i] = i;

    for (uint i = 0; i < cuts.size(); i++){
        uint w = where[costs[i].idx];
        where[list[i]] = w;
        swp(list[i], list[w]);
        swp(cuts[i], cuts[w]);
    }

    // Make sure best cut is MUX feasible:
    if (P.use_fmux){
        uint best_i = UINT_MAX;
        for (uint i = 0; i < cuts.size(); i++){
            if (cuts[i].mux_depth > 0){     // -- let's be a little bit sloppy here for now and include the selector...
                uint d = 0;
                for (uint j = 0; j < cuts[i].size(); j++){
                    Wire w = cuts[i][j] + N;
                    if (cutmap[w].size() != 0)
                        newMax(d, cutmap[w][0].mux_depth);
                }
                cuts[i].mux_depth = d + 1;
            }

            if (best_i == UINT_MAX && cuts[i].mux_depth <= 2)
                best_i = i;
        }
        assert(best_i != UINT_MAX);

        if (best_i != 0)
            swp(cuts[0], cuts[best_i]);
    }

    // Store area flow and arrival time:
    area_est(w) = costs[0].area / fanout_est[w];
    arrival(w) = costs[0].delay;
}


// Applied during instantiation where some 'area_est[w]' is set to zero.
void LutMap::reprioritizeCuts(Wire w, Array<Cut> cuts)
{
    if (!P.reprio) return;

    float best_delay = FLT_MAX;
    float best_area  = FLT_MAX;
    uint  best_i = 0;

    uint allowed_mux_depth = cuts[0].mux_depth;
    for (uint i = 0; i < cuts.size(); i++){
        if (cuts[i].mux_depth > allowed_mux_depth) continue;

        float this_delay = 0.0f;
        float this_area  = 0.0f;
        for (uint j = 0; j < cuts[i].size(); j++){
            Wire v = cuts[i][j] + N;
            this_area += area_est[v];
            newMax(this_delay, arrival[v]);
        }

        if (cuts[i].mux_depth == 0){
            this_area += P.lut_cost[cuts[i].size()];
            this_delay += 1.0f;
        }else{
            this_area += P.mux_cost * cuts[i].mux_depth;
            /**/this_area *= fanout_est[w];
        }

        if (i == 0){
            best_delay = this_delay;
            best_area  = this_area;
        }else if (this_delay > best_delay)
            break;

        if (newMin(best_area, this_area))
            best_i = i;
    }

    if (best_i != 0)
        swp(cuts[0], cuts[best_i]);
}


//mmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmm
// Cut generation:


#include "LutMap_CutGen.icc"


static
float delay(const Cut& cut, const WMap<float>& arrival, Gig& N)
{
    float arr = 0.0f;
    for (uint i = 0; i < cut.size(); i++){
        Wire w = cut[i] + N;
        newMax(arr, arrival[w]);
    }
    return arr + 1.0f;
}


void LutMap::generateCuts(Wire w)
{
    switch (w.type()){
    case gate_Const:    // -- constants should really have been propagated before mapping, but let's allow for them
    case gate_Reset:    // -- not used, but is part of each netlist
    case gate_Box:      // -- treat sequential boxes like a global source
    case gate_PI:
    case gate_FF:
        // Base case -- Global sources:
        cutmap(w) = Array<Cut>(empty_);     // -- only the trivial cut
        area_est(w) = 0;
        arrival(w) = 0;
        break;

    case gate_And:
    case gate_Lut4:
        // Inductive case:
        if (!cutmap[w]){
            Vec<Cut>& cuts = tmp_cuts;
            cuts.clear();   // -- keep last winner
            if (!winner[w].null())
                cuts.push(winner[w]);     // <<==FT cannot push last winner if mux_depth gets too big
            generateCuts_LogicGate(w, cuts);

            cuts_enumerated += cuts.size();
            prioritizeCuts(w, cuts.slice());

            if (!probeRound()){
                cuts.shrinkTo(P.cuts_per_node);
            }else{
                for (uint i = 1; i < cuts.size(); i++){
                    if (delay(cuts[i], arrival, N) > delay(cuts[0], arrival, N))
                        cuts.shrinkTo(i);
                }
                cuts.shrinkTo(2 * P.cuts_per_node);
            }
            cutmap(w) = Array_copy(cuts, mem);
        }else
            prioritizeCuts(w, cutmap[w]);
        break;

    case gate_PO:
    case gate_Seq:
        // Nothing to do.
        break;

    case gate_Bar:
    case gate_Sel:
        // Treat barriers and pin selectors as PIs except for delay:
        cutmap(w) = Array<Cut>(empty_);
        area_est(w) = 0;
        arrival(w) = arrival[w[0]];
        break;

    case gate_Delay:{
        // Treat delay gates as PIs except for delay:
        cutmap(w) = Array<Cut>(empty_);
        area_est(w) = 0;
        float arr = 0;
        For_Inputs(w, v)
            newMax(arr, arrival[v]);
        arrival(w) = arr + w.arg() / DELAY_FRACTION;
        break;}

    default:
        ShoutLn "INTERNAL ERROR! Unhandled gate type: %_", w.type();
        assert(false);
    }
}


//mmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmm
// Exact local area:


struct RefDerefCut {
    const Gig&               N;
    const WMap<Array<Cut> >& cutmap;
    const Params_LutMap&     P;
    WMap<uint>&              fanouts;
    WMap<uchar>&             active;
    Vec<GLit>                undo;

    RefDerefCut(const Gig& N_, const WMap<Array<Cut> >& cutmap_, const Params_LutMap& P_, WMap<uint>& fanouts_, WMap<uchar>& active_) :
        N(N_), cutmap(cutmap_), P(P_), fanouts(fanouts_), active(active_) {}

    uint        acc;
    uint        lim;

    uint derefCut(const Cut& cut, uint lim_ = UINT_MAX) {
        acc = 0;
        lim = lim_;
        undo.clear();
        return deref(cut) ? acc : 0; }

    uint refCut(const Cut& cut, uint lim_ = UINT_MAX) {
        acc = 0;
        lim = lim_;
        undo.clear();
        return ref(cut) ? acc : UINT_MAX; }

    uint tryCut(const Cut& cut, uint lim_ = UINT_MAX) {
        uint ret = refCut(cut, lim_);
        undoRef();
        return ret; }

    void undoDeref();
    void undoRef();

private:
    bool deref(const Cut& cut);
    bool ref(const Cut& cut);
};


bool RefDerefCut::deref(const Cut& cut)
{
    acc += cut.mux_depth ? P.mux_cost * cut.mux_depth : P.lut_cost[cut.size()];
    if (acc > lim) return false;

    for (uint i = 0; i < cut.size(); i++){
        Wire v = cut[i] + N; assert_debug(fanouts[v] > 0); assert_debug(active[v]);
        if (!isLogic(v)) continue;

        fanouts(v)--;
        undo.push(v);
        if (fanouts[v] == 0){
            active(v) = false;
            if (!deref(cutmap[v][0]))
                return false;
        }
    }

    return true;
}


bool RefDerefCut::ref(const Cut& cut)
{
    acc += cut.mux_depth ? P.mux_cost * cut.mux_depth : P.lut_cost[cut.size()];
    if (acc > lim) return false;

    for (uint i = 0; i < cut.size(); i++){
        Wire v = cut[i] + N;
        if (!isLogic(v)) continue;

        fanouts(v)++;   // <<== update (store in undo)
        undo.push(v);
        if (fanouts[v] == 1){
            active(v) = true;   // <<== update (store in undo)
            if (!ref(cutmap[v][0]))
                return false;
        }
    }

    return true;
}


void RefDerefCut::undoDeref()
{
    for (uint i = undo.size(); i > 0;){ i--;
        GLit w = undo[i];
        if (fanouts[w] == 0)
            active(w) = true;
        fanouts(w)++;
    }
}


void RefDerefCut::undoRef()
{
    for (uint i = undo.size(); i > 0;){ i--;
        GLit w = undo[i];
        fanouts(w)--;
        if (fanouts[w] == 0)
            active(w) = false;
    }
}


ZZ_PTimer_Add(LutMap_ELA);


// <<== need to make this delay aware (as well as unmapping)
void LutMap::exactLocalArea(WMap<uint>& fanouts)
{
    // <<==FT should not chose cut with higher mux depth
    ZZ_PTimer_Scope(LutMap_ELA);

    RefDerefCut R(N, cutmap, P, fanouts, active);

  #if defined(ELA_TIMING)
    depart.clear();
  #endif

    For_Gates_Rev(N, w){
        if (active[w] && isLogic(w) && cutmap[w].size() > 1){
#if 1 // ELA
            uint best = R.derefCut(cutmap[w][0], ELA_GLOBAL_LIM);
            if (best <= 1){
                R.undoDeref();
                continue; }

            uint best_i = 0;
            for (uint i = 1; i < cutmap[w].size(); i++){
                Cut& cut = cutmap[w][i];

                if (cut.mux_depth > 0){     // -- let's be a little bit sloppy here for now and include the selector...
                    uint d = 0;
                    for (uint j = 0; j < cut.size(); j++){
                        Wire w = cut[j] + N;
                        if (cutmap[w].size() != 0)
                            newMax(d, cutmap[w][0].mux_depth);
                    }
                    cut.mux_depth = d + 1;
                }

                if (cut.mux_depth > cutmap[w][0].mux_depth) continue;

              #if defined(ELA_TIMING)
                // Check if meets timing:       <<== delay/area trade-off
                float arr = 0;
                for (uint j = 0; j < cut.size(); j++)
                    newMax(arr, arrival[cut[j] + N] + 1.0f);

//                if (arr > arrival[w] && arr + depart[w] > target_arrival + /*FUDGE*/2)
                if (arr > arrival[w] && arr + depart[w] > target_arrival + /*NO FUDGE*/0)
                    continue;
              #endif

                // Try cut:
                uint cost = R.tryCut(cut, best);
                if (newMin(best, cost))
                    best_i = i;
            }
            swp(cutmap[w][0], cutmap[w][best_i]);
            R.refCut(cutmap[w][0]);
#endif
        }

      #if defined(ELA_TIMING)
        // Departure time computation:      <<== flytta till efter valt cut.
        if (isLogic(w)){
            if (active[w]){
                const Cut& cut = cutmap[w][0];
                for (uint i = 0; i < cut.size(); i++){
                    Wire v = cut[i] + N;
                    newMax(depart(v), depart[w] + 1.0f);
                }
            }else
                depart(w) = FLT_MAX;    // -- marks deactivated node

        }else if (!isCI(w)){
            float delay = (w != gate_Delay) ? 0.0f : w.arg() / DELAY_FRACTION;
            For_Inputs(w, v){
                newMax(depart(v), depart[w] + delay);
            }
        }
      #endif
    }
}


//mmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmm
// Fanout estimation:


struct InstOrder {
    const WMap<float>& arrival;
    const WMap<float>& depart;
    InstOrder(const WMap<float>& arrival_, const WMap<float>& depart_) : arrival(arrival_), depart(depart_) {}

    bool operator()(GLit p, GLit q) const {
        float len_p = arrival[p] + depart[p];
        float len_q = arrival[q] + depart[q];
        if (len_p > len_q) return true;
        if (len_p < len_q) return false;
        if (arrival[p] < arrival[q]) return true;
        if (arrival[p] > arrival[q]) return false;
        return false;
    }
};


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

    active.clear();

    InstOrder lt(arrival, depart);
    KeyHeap<GLit, false, InstOrder> ready(lt);
    For_Gates(N, w){
        if (isCO(w)){
            ready.add(w); assert(w);
            active(w) = true;
        }
    }

    while (ready.size() > 0){
        Wire w = ready.pop() + N;
        assert(active[w]);

        if (isLogic(w)){
            reprioritizeCuts(w, cutmap[w]);
            const Cut& cut = cutmap[w][0];
            for (uint i = 0; i < cut.size(); i++){
                Wire v = cut[i] + N;
                area_est(v) = 0;
                if (!active[v]){
                    ready.add(v); assert(v);
                    active(v) = true;
                }
            }

        }else if (!isCI(w)){
            For_Inputs(w, v){
                if (!active[v]){
                    ready.add(v); assert(v);
                    active(v) = true;
                }
            }
        }
    }

    depart.clear();
    For_All_Gates_Rev(N, w){
        if (isLogic(w)){
            if (active[w]){
                const Cut& cut = cutmap[w][0];
                for (uint i = 0; i < cut.size(); i++){
                    Wire v = cut[i] + N;
                    fanouts(v)++;
                    float delay = 1.0f;
                    if (cut.mux_depth > 0){
                        Npn4Norm n = npn4_norm[w.arg()]; assert(n.eq_class == npn4_cl_MUX);
                        pseq4_t seq = perm4_to_pseq4[n.perm];
                        Wire w_sel = w[pseq4Get(seq, 0)];
                        if (w_sel.id != cut[i])
                            delay = 0.0f;
                    }
                    newMax(depart(v), depart[w] + delay);
                }
            }else
                depart(w) = FLT_MAX;    // -- marks deactivated node

        }else if (!isCI(w)){
            float delay = (w != gate_Delay) ? 0.0f : w.arg() / DELAY_FRACTION;
            For_Inputs(w, v){
                fanouts(v)++;
                newMax(depart(v), depart[w] + delay);
            }
        }
    }

    if (round > 0 && P.use_ela)
        exactLocalArea(fanouts);

    mapped_delay = 0.0f;
    For_Gates(N, w)
        if (isCI(w))
            newMax(mapped_delay, depart[w]);

    // -- temporary solution for computing estimated departure for inactive nodes
    WMap<float> tmpdep;
    depart.copyTo(tmpdep);
    For_Gates_Rev(N, w){
        if (isCI(w)) continue;
        if (tmpdep[w] == FLT_MAX) continue;

        //if (isLogic(w)) continue;
        For_Inputs(w, v){
            if (!isLogic(v)) continue;
            if (depart[v] != FLT_MAX) continue;

            if (tmpdep[v] == FLT_MAX) tmpdep(v) = 0;
            newMax(tmpdep(v), tmpdep[w]);
        }
    }

    For_Gates(N, w)
        if (depart[w] == FLT_MAX && tmpdep[w] != FLT_MAX)
            depart(w) = tmpdep[w];

    mapped_area = 0;
    For_Gates(N, w){
        if (active[w] && isLogic(w))
            mapped_area += cutmap[w][0].mux_depth ? P.mux_cost * cutmap[w][0].mux_depth : P.lut_cost[cutmap[w][0].size()];
    }

    if (!instantiate){
        if (!P.map_for_delay || round > 0){
            // Blend new values with old:
            uint  r = round + 1;
            if (P.map_for_delay && round != 0) r -= 1;
            float alpha = 1.0f - 1.0f / (float)(r*r*r*r + 2.0f);
            float beta  = 1.0f - alpha;

            For_Gates(N, w){
                if (isLogic(w)){
                    fanout_est(w) = alpha * max_(fanouts[w], 1u)
//                  fanout_est(w) = alpha * max_(double(fanouts[w]), 0.95)   // -- slightly less than 1 leads to better delay
                                  + beta  * fanout_est[w];
                }
            }
        }

    }else{
        // Compute FTBs:
        uint count = 0;
        For_Gates(N, w)
            if (isLogic(w) && active[w])
                count++;
        Vec<uint64> ftbs(reserve_, count);

        For_Gates(N, w){
            if (isLogic(w) && active[w]){
                const Cut& cut = cutmap[w][0];
                ftbs += computeFtb(w, cut, in_memo, memo);
            }
        }

        // Build LUT representation:
        uint j = 0;
        For_Gates(N, w){
            if (isLogic(w) && active[w]){
                const Cut& cut = cutmap[w][0];
                // Change AND gate into a LUT6:
                change(w, gate_Lut6);
                uint sz = 0;
                for (uint i = 0; i < cut.size(); i++){
                    if (ftb6_inSup(ftbs[j], i)){
                        w.set(sz, cut[i] + N);
                        if (sz < i)
                            ftbs[j] = ftb6_swap(ftbs[j], i, sz);
                        sz++;
                    }
                }
                ftb(w) = ftbs[j++];

                if (cut.mux_depth != 0){
                    // Special F7 or F8 MUX; represent it as a 'Mux':
                    uint64 ftb_ = ftb(w);
                    Npn4Norm n = npn4_norm[ftb_ & 0xFFFF];
                    if (n.eq_class == npn4_cl_MUX){     // -- sometimes the MUX is actually, an OR2, EQU2, BUF, or TRUE...
                        for (uint i = 0; i < 3; i++) assert(ftb6_inSup(ftb_, i));
                        for (uint i = 4; i < 6; i++) assert(!ftb6_inSup(ftb_, i));

                        pseq4_t seq = perm4_to_pseq4[n.perm];
                        Wire w0 = w[pseq4Get(seq, 0)] ^ bool(n.negs & (1 << pseq4Get(seq, 0)));
                        Wire w1 = w[pseq4Get(seq, 2)] ^ bool(n.negs & (1 << pseq4Get(seq, 2)));
                        Wire w2 = w[pseq4Get(seq, 1)] ^ bool(n.negs & (1 << pseq4Get(seq, 1)));

                        change(w, gate_Mux);
                        w.init(w0, w1, w2);
                    }
                    //**/else putchar('!'), fflush(stdout);
                }
            }
        }

        For_Gates_Rev(N, w)
            if (w == gate_And)
                remove(w);
        GigRemap m;
        N.compact(m);
        if (remap)
            m.applyTo(remap->base());

        uint n_luts  = N.typeCount(gate_Lut6);
        uint n_muxes = N.typeCount(gate_Mux);
        if (n_muxes > 0){
            removeMuxViolations(N, arrival, target_arrival, remap);
            WriteLn "  -- Legalizing MUXes by duplication, adding:  #Mux=%_   #Lut6=%_", N.typeCount(gate_Mux) - n_muxes, N.typeCount(gate_Lut6) - n_luts;

            GigRemap m;
            N.compact(m);
            if (remap)
                m.applyTo(remap->base());
        }

#if 1   /*DEBUG*/
        if (P.use_fmux && !P.quiet){
            checkFmuxes(N); }
#endif  /*END DEBUG*/
    }
}


//mmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmm
// Main:


void LutMap::run()
{
    round = 0;
    best_arrival = 0;

    area_est  .reserve(N.size());
    fanout_est.reserve(N.size());
    active    .reserve(N.size());
    depart    .reserve(N.size());

    // Initialize fanout estimation (and zero area estimation):
    {
        Auto_Gob(N, FanoutCount);
        For_Gates(N, w){
            area_est  (w) = 0;
            fanout_est(w) = nFanouts(w);
            active    (w) = true;
            depart    (w) = FLT_MAX;
        }
    }

    // Techmap:
    uint last_round = P.n_rounds - 1 + (uint)P.map_for_delay;
    for (round = 0; round <= last_round; round++){
        double T0 = cpuTime();
        cuts_enumerated = 0;
        For_All_Gates(N, w)
            generateCuts(w);
        double T1 = cpuTime();

        bool instantiate = (round == last_round);
        updateFanoutEst(instantiate);
        double T2 = cpuTime();

        if (round == 0)
            target_arrival = mapped_delay * P.delay_factor;
        newMin(best_arrival, mapped_delay);

        if (!P.quiet)
            WriteLn "phase=%d   mapped_area=%,d   mapped_delay=%_   cuts=%,d   [enum: %t, blend: %t]", round, mapped_area, mapped_delay, cuts_enumerated, T1-T0, T2-T1;

        if (round == 0 || !P.recycle_cuts || (round == 1 && P.map_for_delay)){
            if (round != last_round){
                winner.clear();
                For_Gates(N, w)
                    if (isLogic(w) && cutmap[w].size() > 0)
                        winner(w) = cutmap[w][0];
            }

            for (uint i = 0; i < cutmap.base().size(); i++)
                dispose(cutmap.base()[i], mem);
            cutmap.clear();
        }
    }

    WriteLn "Result: %_", info(N);
}


// <<== put preprocessing here, not in Main_lutmap.cc


LutMap::LutMap(Gig& N_, Params_LutMap P_, WMapX<GLit>* remap_) :
    P(P_), N(N_), remap(remap_)
{
    assert(!N.is_frozen);

    if (Has_Gob(N, Strash))
        Remove_Gob(N, Strash);

    if (!isCanonical(N)){
        WriteLn "Compacting... %_", info(N);

        gate_id orig_sz = N.size();
        GigRemap m;
        N.compact(m);
        if (remap){
            for (gate_id i = 0; i < orig_sz; i++){
                Lit p = GLit(i);
                (*remap)(p) = m(p);
            }
        }
        WriteLn "Done... %_", info(N);

    }else if (remap){
        For_All_Gates(N, w)
            (*remap)(w) = w;
    }

    normalizeLut4s(N);

    // Run mapper:
    run();
  #if 1   /*DEBUG*/
    if (P.use_fmux && !P.quiet){
        checkFmuxes(N); }
  #endif  /*END DEBUG*/

    // Free memory:
    for (uint i = 0; i < cutmap.base().size(); i++)
        dispose(cutmap.base()[i], mem);
    mem.clear(false);

    area_est  .clear(true);
    fanout_est.clear(true);
}


// The 'remap' map will map old gates to new gates (with sign, so 'x' can go to '~y'). Naturally,
// many signals may be gone; these are mapped to 'glit_NULL'. NOTE! Even inputs may be missing from
// 'remap' if they are not in the transitive fanin of any output.
//
void lutMap(Gig& N, Params_LutMap P, WMapX<GLit>* remap)
{
//        expandLut3s(N);
//        introduceMuxesAsLuts(N);
//    putIntoLut4(N); -- if needed

    LutMap inst(N, P, remap);
}


void lutMap(Gig& N, const Vec<Params_LutMap>& Ps, WMapX<GLit>* remap)
{
    WMapX<GLit> xlat;
    xlat.initBuiltins();
    if (remap)
        remap->initBuiltins();

    for (uint i = 0; i < Ps.size(); i++){
        NewLine;
        WriteLn "==== Mapping %_ ====", i + 1;
        lutMap(N, Ps[i], remap ? &xlat : NULL);

        if (remap){
            if (i == 0)
                xlat.moveTo(*remap);
            else{
                Vec<GLit>& v = remap->base();
                for (uint i = gid_FirstUser; i < v.size(); i++){
                    //**/Wire w = N_orig[i];
                    //**/if (!isLogicGate(w) && v[i] && !xlat[v[i]]) WriteLn "LOST: %_ from %_ -> %_", w, v[i] + N_copy, xlat[v[i]];
                    v[i] = xlat[v[i]];
                }
            }
        }

        if (i != Ps.size() - 1 || Ps[LAST].end_with_unmap){
            NewLine;
            WriteLn "==== Unmapping %_ ====", i + 1;
            unmap(N, &xlat);
            N.unstrash();

            if (remap){
                Vec<GLit>& v = remap->base();
                for (uint i = gid_FirstUser; i < v.size(); i++)
                    v[i] = xlat[v[i]];
            }

            GigRemap cmap;
            N.compact(cmap);
            if (remap)
                cmap.applyTo(remap->base());

            WriteLn "Result: %_", info(N);
            WriteLn "Pseudo-ANDs: %_", N.typeCount(gate_And)
                                     + N.typeCount(gate_Xor)  * 3
                                     + N.typeCount(gate_Mux)  * 3
                                     + N.typeCount(gate_Maj)  * 5
                                     + N.typeCount(gate_One)  * 8
                                     + N.typeCount(gate_Gamb) * 5
                                     + N.typeCount(gate_Dot)  * 5;

            if (i != Ps.size() - 1)
                putIntoLut4(N);     // -- keep structured form for final unmapping if this is the end result
        }
    }
    NewLine;

    if (remap){
        Vec<GLit>& v = remap->base();
        for (uint i = gid_FirstUser; i < v.size(); i++)
            if (+v[i] == GLit_NULL)
                v[i] = GLit_NULL;       // -- remove sign on 'GLit_NULL'
    }
    // <<== add profiling timers to see where time is spent
    // <<== perhaps let mapper work directly on GIG rather than in Lut4 mode?

}


//mmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmm
}

/*
Prova att spara 'depart' från iteration till iteration så att omappade noder har en bättre gissning.
Markera omappade noder på annat sätt.
*/

/*
Om inte omgenererar cuts, spara hälften delay-opt, hälften area-opt?
Om växlar snabba (återanvänder cuts) och långsamma faser (nya cuts), hantera fanout-est blendingen annorlunda.
Om genererar nya cuts, spara bästa (eller bästa k stycken) cut från förra fasen.

Departure est. för omappade noder: välj ett cut i termer av mappade noder (om existerar) och tag max: ELLER
loopa över alla cuts i alla instansierade noder och ta max.

Parametrar:
  - arrival-time range
  - area-est range
  - est. slack (incl. "unknown" as a boolean)

+ previous best choice? Or not needed?

Major rounds:
  - blending ratio
  - reuse cuts (boolean)

*/

/*
delay optimal everywhere
globally delay optimal, use slack for area recovery

hur få diversity med? varför svårt att hitta delay optimal med få cuts när Alan lyckas?

<<== FT cut-recycling, sista problemet?
*/
