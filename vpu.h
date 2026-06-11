#pragma once
#include <cstdint>
#include <cstring>
#include <cassert>
#include <cstdio>

// ------------------------------------------------------------------
// Per-instruction payload fields we need stored in PAY.buf[].
// ------------------------------------------------------------------

// ------------------------------------------------------------------
// SVP entry (one row in the Stride Value Predictor table)
// ------------------------------------------------------------------
struct svp_entry_t {
    uint64_t tag;            // PC tag (full, partial, or absent)
    int64_t  retired_value;  // most recent committed value
    int64_t  stride;         // expected delta between consecutive values
    int      instance;       // # speculatively in-flight instances
    int      confidence;     // saturating counter [0 .. conf_max]
    bool     valid;          // entry allocated?
};

// ------------------------------------------------------------------
// VPQ entry (one slot in the Value Prediction Queue circular buffer)
// ------------------------------------------------------------------
struct vpq_entry_t {
    uint64_t pc;             // full PC of the instruction
    int64_t  value;          // actual computed value (deposited at execute)
    bool     value_valid;    // has the execute stage deposited a value?
    bool     allocated;      // is this slot in use?
    bool     svp_hit;        // was there an SVP hit at rename? (for rollback)
};

// ------------------------------------------------------------------
// Result returned from vpu_t::rename_predict()
// ------------------------------------------------------------------
struct vp_prediction_t {
    bool    predicted;       // true iff SVP hit AND confident
    int64_t value;           // predicted value
    int     vpq_idx;         // VPQ slot allocated (-1 = VPQ full or perfect mode)
    bool    svp_hit;         // was there an SVP hit at rename?
};

// ------------------------------------------------------------------
// Measurement buckets 
// ------------------------------------------------------------------
struct vp_measurements_t {
    uint64_t ineligible;     // instructions not eligible for VP
    uint64_t miss;           // eligible but missed in SVP table
    uint64_t conf_corr;      // confident and correct  (IPC benefit)
    uint64_t conf_incorr;    // confident and incorrect (misprediction)
    uint64_t unconf_corr;    // unconfident and correct (lost opportunity)
    uint64_t unconf_incorr;  // unconfident and incorrect
};

// ==================================================================
//  VPU class
// ==================================================================
class vpu_t {
public:
    bool     enabled;         
    bool     perfect;         
    bool     oracle_conf;     
    bool     predINTALU;      
    bool     predFPALU;       
    bool     predLOAD;        

    bool     use_fpc;   // enable Forward Probabilistic Counters
    uint32_t lfsr;      // LFSR state for pseudo-random number generation
    int      n_svp_index_bits; 
    int      n_svp_tag_bits;   
    int      conf_max;         
    int      vpq_size;         

    vp_measurements_t meas;

    vpu_t();
    ~vpu_t();

    void print_config(FILE *fp = stdout) const;
    void init();
    bool is_eligible(bool C_valid, bool is_intalu, bool is_fpalu, bool is_load) const;
    int vpq_free() const;
    int get_vpq_tail() const;
    bool get_vpq_tail_phase() const;
    vp_prediction_t rename_predict(uint64_t pc, bool good_instruction, int64_t oracle_value);
    bool execute_check(int vpq_idx, int64_t computed_value, bool was_predicted, int64_t pred_value);
    void retire_train(uint64_t pc_from_pay, bool was_predicted, int64_t pred_value, int64_t oracle_value, bool good_instr, bool svp_hit);
    void retire_ineligible();
    void partial_rollback(int checkpoint_vpq_tail, bool checkpoint_vpq_tail_phase);
    void full_rollback();
    uint64_t svp_storage_bytes() const;
    void print_storage(FILE *fp = stdout) const;
    void print_measurements(FILE *fp = stdout) const;

private:
    svp_entry_t  *svp_table;
    unsigned int  svp_size;
    uint64_t      svp_index_mask;
    uint64_t      svp_tag_mask;

    vpq_entry_t *vpq_buf;
    int          vpq_head;
    int          vpq_tail;
    bool         vpq_head_phase;  
    bool         vpq_tail_phase;  

    unsigned int svp_index(uint64_t pc) const;
    uint64_t svp_tag(uint64_t pc) const;
    bool svp_tag_match(const svp_entry_t *e, uint64_t pc) const;
    svp_entry_t *svp_lookup(uint64_t pc);
    svp_entry_t *svp_slot(uint64_t pc);
    int vpq_count_instances(uint64_t pc) const;
    int conf_bits_needed() const;
    int instance_bits_needed() const;
    bool fpc_should_increment(int current_conf);
    uint32_t lfsr_next();
};
