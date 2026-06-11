#include "vpu.h"

vpu_t::vpu_t() : enabled(false), perfect(false), oracle_conf(false),
          predINTALU(false), predFPALU(false), predLOAD(false),
	  use_fpc(false), lfsr(0xdeadbeef),
          n_svp_index_bits(0), n_svp_tag_bits(0), conf_max(3), vpq_size(0),
          svp_table(nullptr), svp_size(0), svp_index_mask(0), svp_tag_mask(0),
          vpq_buf(nullptr), vpq_head(0), vpq_tail(0),
          vpq_head_phase(false), vpq_tail_phase(false) {
    memset(&meas, 0, sizeof(meas));
}

vpu_t::~vpu_t() {
    delete[] svp_table;
    delete[] vpq_buf;
}

void vpu_t::print_config(FILE *fp) const {
    fprintf(fp, "\n");
    fprintf(fp, "=== VALUE PREDICTOR ============================================================\n");
    fprintf(fp, "\n");
    fprintf(fp, "VP-eligible configuration:\n");
    fprintf(fp, "   predINTALU = %d\n", predINTALU ? 1 : 0);
    fprintf(fp, "   predFPALU  = %d\n", predFPALU  ? 1 : 0);
    fprintf(fp, "   predLOAD   = %d\n", predLOAD   ? 1 : 0);
    fprintf(fp, "\n");

    if (perfect) {
        fprintf(fp, "VALUE PREDICTOR = perfect\n");
        fprintf(fp, "\n");
        fprintf(fp, "COST ACCOUNTING\n");
        fprintf(fp, "  Impossible.\n");
    } else if (enabled) {
        fprintf(fp, "VALUE PREDICTOR = stride (Project 4 spec. implementation)\n");
        fprintf(fp, "   VPQsize         = %d\n", vpq_size);
        fprintf(fp, "   oracleconf      = %d (%s)\n",
                oracle_conf ? 1 : 0,
                oracle_conf ? "oracle confidence" : "real confidence");
        fprintf(fp, "   # index bits    = %d\n", n_svp_index_bits);
        fprintf(fp, "   # tag bits      = %d\n", n_svp_tag_bits);
        fprintf(fp, "   confmax         = %d\n", conf_max);
	if (use_fpc) 
		fprintf(fp, "   fpc             = %d\n", use_fpc ? 1 : 0);  // ← ADD
        fprintf(fp, "\n");
        fprintf(fp, "COST ACCOUNTING\n");
        print_storage(fp);
    } else {
        fprintf(fp, "VALUE PREDICTOR = none\n");
    }
}

void vpu_t::init() {
    if (!enabled) return;

    if (perfect) {
        svp_size = 0;
        return;
    }

    svp_size       = 1u << n_svp_index_bits;
    svp_index_mask = svp_size - 1;

    if (n_svp_tag_bits > 0) {
        svp_tag_mask = ((1ULL << n_svp_tag_bits) - 1);
    } else {
        svp_tag_mask = 0;
    }

    svp_table = new svp_entry_t[svp_size]();

    if (n_svp_tag_bits == 0) {
        for (unsigned int i = 0; i < svp_size; i++) {
            svp_table[i].valid         = true;
            svp_table[i].tag           = 0;
            svp_table[i].retired_value = 0;
            svp_table[i].stride        = 0;
            svp_table[i].confidence    = 0;
            svp_table[i].instance      = 0;
        }
    }

    assert(vpq_size > 0);
    vpq_buf        = new vpq_entry_t[vpq_size]();
    vpq_head       = 0;
    vpq_tail       = 0;
    vpq_head_phase = false;
    vpq_tail_phase = false;
}

bool vpu_t::is_eligible(bool C_valid, bool is_intalu, bool is_fpalu, bool is_load) const {
    if (!C_valid) return false;
    if (is_intalu && predINTALU) return true;
    if (is_fpalu  && predFPALU)  return true;
    if (is_load   && predLOAD)   return true;
    return false;
}

int vpu_t::vpq_free() const {
    if (vpq_head_phase == vpq_tail_phase) {
        return vpq_size - (vpq_tail - vpq_head);
    } else {
        return vpq_head - vpq_tail;
    }
}

int  vpu_t::get_vpq_tail()       const { return vpq_tail; }
bool vpu_t::get_vpq_tail_phase() const { return vpq_tail_phase; }

vp_prediction_t vpu_t::rename_predict(uint64_t pc,
                               bool good_instruction,
                               int64_t oracle_value) {
    vp_prediction_t result;
    int64_t pred_val = 0;
    result.predicted = false;
    result.value     = 0;
    result.vpq_idx   = -1;
    result.svp_hit   = false;

    if (perfect) {
        if (good_instruction) {
            result.predicted = true;
            result.value     = oracle_value;
        }
        return result;
    }

    if (vpq_head_phase != vpq_tail_phase && vpq_head == vpq_tail) {
        return result;
    }

    int idx = vpq_tail;
    vpq_buf[idx].pc          = pc;
    vpq_buf[idx].value       = 0;
    vpq_buf[idx].value_valid = false;
    vpq_buf[idx].allocated   = true;
    vpq_buf[idx].svp_hit     = false;
    vpq_tail++;
    if (vpq_tail == vpq_size) {
        vpq_tail       = 0;
        vpq_tail_phase = !vpq_tail_phase;
    }
    result.vpq_idx = idx;

    svp_entry_t *e = svp_lookup(pc);

    if (e == nullptr) {
        vpq_buf[idx].svp_hit = false;
        result.svp_hit       = false;
        return result;
    }

    vpq_buf[idx].svp_hit = true;
    result.svp_hit       = true;
    e->instance++;
    pred_val = e->retired_value + (int64_t)e->instance * e->stride;

    bool confident = false;
    if (oracle_conf) {
        if (good_instruction)
            confident = (pred_val == oracle_value);
        else
            confident = false;
    } else {
        confident = (e->confidence >= conf_max);
    }

    result.predicted = confident;
    result.value     = pred_val;
    return result;
}

bool vpu_t::execute_check(int vpq_idx, int64_t computed_value,
                   bool was_predicted, int64_t pred_value) {
    if (vpq_idx >= 0 && vpq_buf[vpq_idx].allocated) {
        vpq_buf[vpq_idx].value       = computed_value;
        vpq_buf[vpq_idx].value_valid = true;
    }

    if (!was_predicted) return false;

    return (computed_value != pred_value);
}

void vpu_t::retire_train(uint64_t /*pc_from_pay*/, bool was_predicted,
                  int64_t pred_value, int64_t oracle_value,
                  bool good_instr, bool svp_hit) {

    int64_t  actual_value;
    uint64_t pc;

    if (!perfect) {
        if (vpq_head_phase == vpq_tail_phase && vpq_head == vpq_tail) {
            return;
        }

        vpq_entry_t head = vpq_buf[vpq_head];
        vpq_buf[vpq_head].allocated = false;
        vpq_head++;
        if (vpq_head == vpq_size) {
            vpq_head       = 0;
            vpq_head_phase = !vpq_head_phase;
        }

        assert(head.allocated);
        assert(head.value_valid);

        pc           = head.pc;
        actual_value = head.value;

    } else {
        actual_value = oracle_value;
        pc           = 0;
    }

    if (!was_predicted) {
        if (!perfect) {
            if (!svp_hit) {
                meas.miss++;
            } else {
                bool would_be_correct = (pred_value == actual_value);
                if (would_be_correct)
                    meas.unconf_corr++;
                else
                    meas.unconf_incorr++;
            }
        }
    } else {
        bool pred_correct = (pred_value == actual_value);
        if (pred_correct)
            meas.conf_corr++;
        else
            meas.conf_incorr++;
    }

    if (perfect) return;

    svp_entry_t *e = svp_lookup(pc);
    if (e == nullptr) {
        svp_entry_t *slot = svp_slot(pc);
        slot->tag           = svp_tag(pc);
        slot->retired_value = actual_value;
        slot->stride        = actual_value;
        slot->confidence    = 0;
        slot->valid         = true;
        slot->instance      = vpq_count_instances(pc);
    } else {
        int64_t new_stride = actual_value - e->retired_value;
        if (new_stride == e->stride) {
            if (e->confidence < conf_max) 
		    if (e->confidence < conf_max && fpc_should_increment(e->confidence))
		      e->confidence++;
        } else {
            e->stride     = new_stride;
            e->confidence = 0;
        }
        e->retired_value = actual_value;
        if (e->instance > 0) e->instance--;
    }
}

void vpu_t::retire_ineligible() {
    meas.ineligible++;
}

void vpu_t::partial_rollback(int checkpoint_vpq_tail, bool checkpoint_vpq_tail_phase) {
    int  pos       = checkpoint_vpq_tail;
    bool pos_phase = checkpoint_vpq_tail_phase;

    while (pos != vpq_tail || pos_phase != vpq_tail_phase) {
        if (vpq_buf[pos].allocated) {
            if (vpq_buf[pos].svp_hit) {
                svp_entry_t *e = svp_lookup(vpq_buf[pos].pc);
                if (e && e->instance > 0) e->instance--;
            }
            vpq_buf[pos].allocated = false;
        }
        pos++;
        if (pos == vpq_size) {
            pos       = 0;
            pos_phase = !pos_phase;
        }
    }

    vpq_tail       = checkpoint_vpq_tail;
    vpq_tail_phase = checkpoint_vpq_tail_phase;
}

void vpu_t::full_rollback() {
    for (unsigned int i = 0; i < svp_size; i++) {
        if (svp_table[i].valid) svp_table[i].instance = 0;
    }
    vpq_tail       = vpq_head;
    vpq_tail_phase = vpq_head_phase;
}

uint64_t vpu_t::svp_storage_bytes() const {
    if (!enabled || perfect) return 0;
    int conf_bits      = conf_bits_needed();
    int instance_bits  = instance_bits_needed();
    int bits_per_entry = n_svp_tag_bits + conf_bits + 64 + 64 + instance_bits;
    int bytes_per_entry = (bits_per_entry + 7) / 8;
    return (uint64_t)svp_size * bytes_per_entry;
}

void vpu_t::print_storage(FILE *fp) const {
    if (!enabled || perfect) return;

    int conf_bits      = conf_bits_needed();
    int instance_bits  = instance_bits_needed();
    int bits_per_entry = n_svp_tag_bits + conf_bits + 64 + 64 + instance_bits;
    int bytes_per_entry = (bits_per_entry + 7) / 8;
    double total_bytes  = (double)svp_size * bytes_per_entry;
    double total_kb     = total_bytes / 1024.0;

    fprintf(fp, "   One SVP entry:\n");
    fprintf(fp, "      tag           : %3d bits  // num_tag_bits\n",
            n_svp_tag_bits);
    fprintf(fp, "      conf          : %3d bits  // formula: (uint64_t)ceil(log2((double)(confmax+1)))\n",
            conf_bits);
    fprintf(fp, "      retired_value : %3d bits  // RISCV64 integer size.\n", 64);
    fprintf(fp, "      stride        : %3d bits  // RISCV64 integer size. Competition opportunity: truncate stride to far fewer bits based on stride distribution of stride-predictable instructions.\n", 64);
    fprintf(fp, "      instance ctr  : %3d bits  // formula: (uint64_t)ceil(log2((double)VPQsize))\n",
            instance_bits);
    fprintf(fp, "      -------------------------\n");
    fprintf(fp, "      bits/SVP entry: %3d bits\n", bits_per_entry);
    fprintf(fp, "   Total storage cost (bits) = (%u SVP entries x %d bits/SVP entry) = %llu bits\n",
            svp_size, bits_per_entry,
            (unsigned long long)svp_size * bits_per_entry);
    fprintf(fp, "   Total storage cost (bytes) = %.2f B (%.2f KB)\n",
            total_bytes, total_kb);
}

void vpu_t::print_measurements(FILE *fp) const {
    uint64_t eligible = meas.miss + meas.conf_corr + meas.conf_incorr
                      + meas.unconf_corr + meas.unconf_incorr;
    uint64_t total = meas.ineligible + eligible;

    double pct_ineligible    = total > 0 ? 100.0 * meas.ineligible    / total : 0.0;
    double pct_eligible      = total > 0 ? 100.0 * eligible            / total : 0.0;
    double pct_miss          = total > 0 ? 100.0 * meas.miss           / total : 0.0;
    double pct_conf_corr     = total > 0 ? 100.0 * meas.conf_corr      / total : 0.0;
    double pct_conf_incorr   = total > 0 ? 100.0 * meas.conf_incorr    / total : 0.0;
    double pct_unconf_corr   = total > 0 ? 100.0 * meas.unconf_corr    / total : 0.0;
    double pct_unconf_incorr = total > 0 ? 100.0 * meas.unconf_incorr  / total : 0.0;

    fprintf(fp, "VPU MEASUREMENTS-----------------------------------\n");
    fprintf(fp, "vpmeas_ineligible         : %10llu (%6.2f%%) // Not eligible for value prediction.\n",
            (unsigned long long)meas.ineligible, pct_ineligible);
    fprintf(fp, "vpmeas_eligible           : %10llu (%6.2f%%) // Eligible for value prediction.\n",
            (unsigned long long)eligible, pct_eligible);
    fprintf(fp, "   vpmeas_miss            : %10llu (%6.2f%%) // VPU was unable to generate a value prediction (e.g., SVP miss).\n",
            (unsigned long long)meas.miss, pct_miss);
    fprintf(fp, "   vpmeas_conf_corr       : %10llu (%6.2f%%) // VPU generated a confident and correct value prediction.\n",
            (unsigned long long)meas.conf_corr, pct_conf_corr);
    fprintf(fp, "   vpmeas_conf_incorr     : %10llu (%6.2f%%) // VPU generated a confident and incorrect value prediction. (MISPREDICTION)\n",
            (unsigned long long)meas.conf_incorr, pct_conf_incorr);
    fprintf(fp, "   vpmeas_unconf_corr     : %10llu (%6.2f%%) // VPU generated an unconfident and correct value prediction. (LOST OPPORTUNITY)\n",
            (unsigned long long)meas.unconf_corr, pct_unconf_corr);
    fprintf(fp, "   vpmeas_unconf_incorr   : %10llu (%6.2f%%) // VPU generated an unconfident and incorrect value prediction.\n",
	    (unsigned long long)meas.unconf_incorr, pct_unconf_incorr);
}

unsigned int vpu_t::svp_index(uint64_t pc) const {
    return (unsigned int)((pc >> 2) & svp_index_mask);
}

uint64_t vpu_t::svp_tag(uint64_t pc) const {
    if (n_svp_tag_bits == 0) return 0;
    return (pc >> (2 + n_svp_index_bits)) & svp_tag_mask;
}

bool vpu_t::svp_tag_match(const svp_entry_t *e, uint64_t pc) const {
    if (n_svp_tag_bits == 0) return true;
    return e->valid && (e->tag == svp_tag(pc));
}

svp_entry_t *vpu_t::svp_lookup(uint64_t pc) {
    svp_entry_t *e = &svp_table[svp_index(pc)];
    if (!e->valid) return nullptr;
    if (!svp_tag_match(e, pc)) return nullptr;
    return e;
}

svp_entry_t *vpu_t::svp_slot(uint64_t pc) {
    return &svp_table[svp_index(pc)];
}

int vpu_t::vpq_count_instances(uint64_t pc) const {
    int cnt = 0;
    int pos = vpq_head;
    bool pos_phase = vpq_head_phase;
    int n = 0;
    while (pos != vpq_tail || pos_phase != vpq_tail_phase) {
        if (vpq_buf[pos].allocated && vpq_buf[pos].pc == pc) cnt++;
        if (pos == vpq_size - 1) pos_phase = !pos_phase;
        pos = (pos + 1) % vpq_size;
        n++;
        if (n > vpq_size) break;
    }
    return cnt;
}

// LFSR-based pseudo-random number generator
uint32_t vpu_t::lfsr_next() {
    lfsr ^= lfsr << 13;
    lfsr ^= lfsr >> 17;
    lfsr ^= lfsr << 5;
    return lfsr;
}

// FPC: should we increment confidence counter?
// Probability vector: {1, 1/16, 1/16, 1/16, 1/16, 1/32, 1/32}
// Mimics a 7-bit counter using only 3 bits
bool vpu_t::fpc_should_increment(int current_conf) {
    if (!use_fpc) return true;  // normal: always increment
    switch (current_conf) {
        case 0: return true;                        // prob = 1
        case 1: return (lfsr_next() & 0xF)  == 0;  // prob = 1/16
        case 2: return (lfsr_next() & 0xF)  == 0;  // prob = 1/16
        case 3: return (lfsr_next() & 0xF)  == 0;  // prob = 1/16
        case 4: return (lfsr_next() & 0xF)  == 0;  // prob = 1/16
        case 5: return (lfsr_next() & 0x1F) == 0;  // prob = 1/32
        case 6: return (lfsr_next() & 0x1F) == 0;  // prob = 1/32
        default: return false;
    }
}

int vpu_t::conf_bits_needed() const {
    if (conf_max == 0) return 0;
    int bits = 1;
    int v = conf_max;
    while (v > 1) { v >>= 1; bits++; }
    return bits;
}

int vpu_t::instance_bits_needed() const {
    if (vpq_size <= 1) return 0;
    int bits = 1;
    int v = vpq_size - 1;
    while (v > 1) { v >>= 1; bits++; }
    return bits;
}
