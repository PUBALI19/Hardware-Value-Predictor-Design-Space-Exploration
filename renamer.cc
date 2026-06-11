#include "renamer.h"
#include <cstdlib>
#include <cstring>

//constructor
renamer::renamer(uint64_t n_log_regs,
                 uint64_t n_phys_regs,
                 uint64_t n_branches,
                 uint64_t n_active)
{
    if (!(n_phys_regs > n_log_regs))
    {
        exit(1);
    }
    if (!(n_branches >= 1 && n_branches <= 64))
    {
        exit(1);
    }
    if (!(n_active > 0))
    {
        exit(1);
    }

    this->n_log_regs  = n_log_regs;
    this->n_phys_regs = n_phys_regs;
    this->n_branches  = n_branches;
    this->n_active    = n_active;

    //initialisation of rmt
    rename_map_table = new uint64_t[n_log_regs];

    //initialisation of amt 
    arch_map_table = new uint64_t[n_log_regs];

    //rmt and amt initilally contain the logical registers
    for (uint64_t i = 0; i < n_log_regs; i++) 
    {
        rename_map_table[i] = i;
        arch_map_table[i] = i;
    }

    //initialisation of prf
    prf_struct = new uint64_t[n_phys_regs]();

    // initialisation of prf_ready_bits array 
    prf_rdy_bits = new bool[n_phys_regs];
    //initilally all registers from 0 to n_phys_regs-1 will be ready
    for (uint64_t i = 0; i < n_phys_regs; i++)
        prf_rdy_bits[i] = true;

    free_list_size = n_phys_regs - n_log_regs;
    free_list = new uint64_t[free_list_size];
    for (uint64_t i = 0; i < free_list_size; i++)
        free_list[i] = n_log_regs + i;   //these are the physical regsiters apart from the committed ones
    free_list_head = 0;
    free_list_tail = 0;          
    free_list_head_phase = false;
    free_list_tail_phase = true; //initially full hence head_phase is not equal to tail_phase

    //initialisation of active list 
    active_list = new active_list_contents[n_active];
    active_list_head = 0;
    active_list_tail = 0;
    active_list_head_phase = false;
    active_list_tail_phase = false;

    GBM = 0;

    //initialisation of branch checkpoints
    branch_checkpts = new br_checkpt[n_branches];
    for (uint64_t i = 0; i < n_branches; i++) 
    {
        branch_checkpts[i].shadow_map_table = new uint64_t[n_log_regs];
    }
}

//destructor
renamer::~renamer()
{
    delete[] rename_map_table;
    delete[] arch_map_table;
    delete[] prf_struct;
    delete[] prf_rdy_bits;
    delete[] free_list;
    delete[] active_list;
    for (uint64_t i = 0; i < n_branches; i++)
        delete[] branch_checkpts[i].shadow_map_table;
    delete[] branch_checkpts;
}

//rename

bool renamer::stall_reg(uint64_t bundle_dst)
{
    return free_regs_fl() < bundle_dst;
}

uint64_t renamer::free_gbm() 
{
    uint64_t count = 0;
    uint64_t mask = GBM;
    for (uint64_t i = 0; i < n_branches; i++) 
    {
        if (!(mask & (1ULL << i))) count++;
    }
    return count;
}
bool renamer::stall_branch(uint64_t bundle_branch)
{
    return free_gbm() < bundle_branch;
}

uint64_t renamer::get_branch_mask()
{
    return GBM;
}

uint64_t renamer::rename_rsrc(uint64_t log_reg)
{
    
    if (!(log_reg < n_log_regs))
    {
        exit(1);
    }
    return rename_map_table[log_reg];
}

uint64_t renamer::rename_rdst(uint64_t log_reg)
{
    if (!(log_reg < n_log_regs))
    {
        exit(1);
    }
    if (!(free_regs_fl() > 0))
    {
        exit(1);
    }

    //free_list_head is popped first
    uint64_t phys_reg = free_list[free_list_head];
    free_list_head++;
    if (free_list_head == free_list_size) 
    {
        free_list_head = 0;
        free_list_head_phase = !free_list_head_phase;
    }

    //rename_map_table is indexed with log_reg and updated
    rename_map_table[log_reg] = phys_reg;

    //the new physical registers ready bit is set to 0
    prf_rdy_bits[phys_reg] = false;

    return phys_reg;
}

uint64_t renamer::checkpoint()
{
    // Find a free bit (0) in the GBM
    uint64_t branch_ID = 0;
    bool found = false;
    for (uint64_t i = 0; i < n_branches; i++) 
    {
        if (!(GBM & (1ULL << i))) 
    {
            branch_ID = i;
            found = true;
            break;
        }
    }
    if (!(found))
    {
        exit(1);
    }

    direct_table_copy(branch_checkpts[branch_ID].shadow_map_table, rename_map_table);
    branch_checkpts[branch_ID].free_list_head       = free_list_head;
    branch_checkpts[branch_ID].free_list_head_phase = free_list_head_phase;
    branch_checkpts[branch_ID].GBM           = GBM;

    GBM |= (1ULL << branch_ID);

    return branch_ID;
}

//dispatch

bool renamer::stall_dispatch(uint64_t bundle_inst)
{
    return free_regs_al() < bundle_inst;
}

uint64_t renamer::dispatch_inst(bool dest_valid,
                                uint64_t log_reg,
                                uint64_t phys_reg,
                                bool load,
                                bool store,
                                bool branch,
                                bool amo,
                                bool csr,
                                uint64_t PC)
{
    if (!(free_regs_al() > 0))
    {
        exit(1);
    }

    uint64_t index = active_list_tail;

    active_list_contents &e = active_list[index];
    e.dest_valid = dest_valid;
    e.log_reg    = log_reg;
    e.phys_reg   = phys_reg;
    e.completed  = false;
    e.exception  = false;
    e.load_viol  = false;
    e.br_misp    = false;
    e.val_misp   = false;
    e.load       = load;
    e.store      = store;
    e.branch     = branch;
    e.amo        = amo;
    e.csr        = csr;
    e.PC         = PC;

    // PROJECT 4: Default initialization for value prediction
    e.val_pred_valid = false;
    e.val_pred_value = 0;

    active_list_tail++;
    if (active_list_tail == n_active) 
    {
        active_list_tail = 0;
        active_list_tail_phase = !active_list_tail_phase;
    }

    return index;
}

bool renamer::is_ready(uint64_t phys_reg)
{
    if (!(phys_reg < n_phys_regs))
    {
        exit(1);
    }
    return prf_rdy_bits[phys_reg];
}

void renamer::clear_ready(uint64_t phys_reg)
{
    if (!(phys_reg < n_phys_regs))
    {
        exit(1);
    }
    prf_rdy_bits[phys_reg] = false;
}

//register read

uint64_t renamer::read(uint64_t phys_reg)
{
    if (!(phys_reg < n_phys_regs))
    {
        exit(1);
    }
    return prf_struct[phys_reg];
}

void renamer::set_ready(uint64_t phys_reg)
{
    if (!(phys_reg < n_phys_regs))
    {
        exit(1);
    }
    prf_rdy_bits[phys_reg] = true;
}

//writeback

void renamer::write(uint64_t phys_reg, uint64_t value)
{
    if (!(phys_reg < n_phys_regs))
    {
        exit(1);
    }
    // Note: Project 4 Slide 45 mentioned a simulator hack where renamer::write() 
    // is called at the end of Execute. This will update the PRF.
    prf_struct[phys_reg] = value;
}

void renamer::set_complete(uint64_t AL_index)
{
    if (!(AL_index < n_active))
    {
        exit(1);
    }
    active_list[AL_index].completed = true;
}

void renamer::resolve(uint64_t AL_index,
                      uint64_t branch_ID,
                      bool correct)
{
    if (correct) 
    {
        //if it is a correct prediction; we need to just clear the branch's bit everywhere
        GBM &= ~(1ULL << branch_ID);
        for (uint64_t i = 0; i < n_branches; i++) 
    {
            if (GBM & (1ULL << i)) 
        {
                branch_checkpts[i].GBM &= ~(1ULL << branch_ID);
            }
        }
    } 
    else 
    {
        // Misprediction: full recovery
        // if it is a misprediction; we need to do full recovery

        GBM = branch_checkpts[branch_ID].GBM;
        GBM &= ~(1ULL << branch_ID);

        direct_table_copy(rename_map_table, branch_checkpts[branch_ID].shadow_map_table);

        free_list_head        = branch_checkpts[branch_ID].free_list_head;
        free_list_head_phase = branch_checkpts[branch_ID].free_list_head_phase;

        uint64_t new_tail = AL_index + 1;
        if (new_tail == n_active)
            new_tail = 0;

        active_list_tail = new_tail;
        //tail_phase is the same as head_phase
        if (new_tail > active_list_head) 
    {
            active_list_tail_phase = active_list_head_phase;
        } 
    //tail_phase is the opposite of head_phase
    else if (new_tail <= active_list_head) 
    {
            active_list_tail_phase = !active_list_head_phase;
        }

    }
}

//retire

bool renamer::precommit(bool &completed,bool &exception, bool &load_viol, bool &br_misp, bool &val_misp,bool &load, bool &store, bool &branch, bool &amo, bool &csr,uint64_t &PC)
{
    //check if active list is empty or not
    if (active_list_head_phase == active_list_tail_phase && active_list_head == active_list_tail)
        return false;

    const active_list_contents &e = active_list[active_list_head];
    completed = e.completed;
    exception = e.exception;
    load_viol = e.load_viol;
    br_misp   = e.br_misp;
    val_misp  = e.val_misp;
    load      = e.load;
    store     = e.store;
    branch    = e.branch;
    amo       = e.amo;
    csr       = e.csr;
    PC        = e.PC;
    return true;
}

void renamer::commit()
{
    if (active_list_head_phase == active_list_tail_phase && active_list_head == active_list_tail)
    {
        exit(1);
    }
    if (!(active_list[active_list_head].completed))
    {
        exit(1);
    }
    if (active_list[active_list_head].exception)
    {
        exit(1);
    }
    if (active_list[active_list_head].load_viol)
    {
        exit(1);
    }

    const active_list_contents &e = active_list[active_list_head];

    if (e.dest_valid) 
    {
        uint64_t old_phys = arch_map_table[e.log_reg];

    // arch_map_table is updated with new committed registers
        arch_map_table[e.log_reg] = e.phys_reg;

    //to make sure that there is space available
        if (!(free_regs_fl() < free_list_size))
    {
        exit(1);
    }
        free_list[free_list_tail] = old_phys;
        free_list_tail++;
        if (free_list_tail == free_list_size) 
    {
            free_list_tail = 0;
            free_list_tail_phase = !free_list_tail_phase;
        }
    }

    active_list_head++;
    if (active_list_head == n_active) 
    {
        active_list_head = 0;
        active_list_head_phase = !active_list_head_phase;
    }
}

void renamer::squash()
{
    //copy amt onto rmt
    direct_table_copy(rename_map_table, arch_map_table);

    bool *in_arch_map_table = new bool[n_phys_regs]();
    for (uint64_t i = 0; i < n_log_regs; i++)
        in_arch_map_table[arch_map_table[i]] = true;

    //make free list from the beginning
    free_list_head = 0;
    free_list_tail = 0;
    free_list_head_phase = false;
    free_list_tail_phase = false;
    
    for (uint64_t p = 0; p < n_phys_regs; p++) 
    {
        if (!in_arch_map_table[p]) 
    {
            free_list[free_list_tail] = p;
            free_list_tail++;
            if (free_list_tail == free_list_size) 
        {
                free_list_tail = 0;
                free_list_tail_phase = !free_list_tail_phase;
            }
        }
    }
    delete[] in_arch_map_table;
    

    //we need to empty the active list
    active_list_tail       = active_list_head;
    active_list_tail_phase = active_list_head_phase;

    //we need to restore all branches after a squash
    GBM = 0;
}

//flags

void renamer::set_exception(uint64_t AL_index)
{
    if (!(AL_index < n_active))
    {
        exit(1);
    }
    active_list[AL_index].exception = true;
}

void renamer::set_load_violation(uint64_t AL_index)
{
    if (!(AL_index < n_active))
    {
        exit(1);
    }
    active_list[AL_index].load_viol = true;
}

void renamer::set_branch_misprediction(uint64_t AL_index)
{
    if (!(AL_index < n_active))
    {
        exit(1);
    }
    active_list[AL_index].br_misp = true;
}

void renamer::set_value_misprediction(uint64_t AL_index)
{
    if (!(AL_index < n_active))
    {
        exit(1);
    }
    active_list[AL_index].val_misp = true;
}

bool renamer::get_exception(uint64_t AL_index)
{
    if (!(AL_index < n_active))
    {
        exit(1);
    }
    return active_list[AL_index].exception;
}

/////////////////////////////////////////////////////////////////////
// PROJECT 4: Value Prediction Support Functions
/////////////////////////////////////////////////////////////////////

void renamer::dispatch_value_prediction(uint64_t AL_index, uint64_t predicted_value) {
    if (!(AL_index < n_active)) {
        exit(1);
    }
    active_list[AL_index].val_pred_valid = true;
    active_list[AL_index].val_pred_value = predicted_value;
}

void renamer::read_predicted_value(uint64_t AL_index, bool &valid, uint64_t &value) {
    if (!(AL_index < n_active)) {
        exit(1);
    }
    valid = active_list[AL_index].val_pred_valid;
    value = active_list[AL_index].val_pred_value;
}
