#include "pipeline.h"


void pipeline_t::squash_complete(reg_t jump_PC) {
   unsigned int i, j;

   //////////////////////////
   // Fetch Stage
   //////////////////////////

   FetchUnit->flush(jump_PC);

   //////////////////////////
   // Decode Stage
   //////////////////////////

   for (i = 0; i < fetch_width; i++) {
      DECODE[i].valid = false;
   }

   //////////////////////////
   // Rename1 Stage
   //////////////////////////

   FQ.flush();

   //////////////////////////
   // Rename2 Stage
   //////////////////////////

   for (i = 0; i < dispatch_width; i++) {
      RENAME2[i].valid = false;
   }

   //
   // FIX_ME #17c
   // Squash the renamer.
   //

   // FIX_ME #17c BEGIN
   REN->squash();
   // FIX_ME #17c END

   // VP BEGIN – full VPQ rollback on complete squash
   // This clears all instance counters and empties the VPQ.
   if (VPU.enabled) {
      VPU.full_rollback();
   }
   // VP END
  
   //////////////////////////
   // Dispatch Stage
   //////////////////////////

   for (i = 0; i < dispatch_width; i++) {
      DISPATCH[i].valid = false;
   }

   //////////////////////////
   // Schedule Stage
   //////////////////////////

   IQ.flush();

   //////////////////////////
   // Register Read Stage
   // Execute Stage
   // Writeback Stage
   //////////////////////////

   for (i = 0; i < issue_width; i++) {
      Execution_Lanes[i].rr.valid = false;
      for (j = 0; j < Execution_Lanes[i].ex_depth; j++)
         Execution_Lanes[i].ex[j].valid = false;
      Execution_Lanes[i].wb.valid = false;
   }

   LSU.flush();
}


void pipeline_t::resolve(unsigned int branch_ID, bool correct) {
   unsigned int i, j;

   if (correct) {
      // Instructions in the Rename2 through Writeback Stages have branch masks.
      // The correctly-resolved branch's bit must be cleared in all branch masks.

      for (i = 0; i < dispatch_width; i++) {
         // Rename2 Stage:
         CLEAR_BIT(RENAME2[i].branch_mask, branch_ID);

         // Dispatch Stage:
         CLEAR_BIT(DISPATCH[i].branch_mask, branch_ID);
      }

      // Schedule Stage:
      IQ.clear_branch_bit(branch_ID);

      for (i = 0; i < issue_width; i++) {
         // Register Read Stage:
         CLEAR_BIT(Execution_Lanes[i].rr.branch_mask, branch_ID);

         // Execute Stage:
         for (j = 0; j < Execution_Lanes[i].ex_depth; j++)
            CLEAR_BIT(Execution_Lanes[i].ex[j].branch_mask, branch_ID);

         // Writeback Stage:
         CLEAR_BIT(Execution_Lanes[i].wb.branch_mask, branch_ID);
      }
   }
   else {
      // Squash all instructions in the Decode through Dispatch Stages.

      // Decode Stage:
      for (i = 0; i < fetch_width; i++) {
         DECODE[i].valid = false;
      }

      // Rename1 Stage:
      FQ.flush();

      // Rename2 Stage:
      for (i = 0; i < dispatch_width; i++) {
         RENAME2[i].valid = false;
      }

      // Dispatch Stage:
      for (i = 0; i < dispatch_width; i++) {
         DISPATCH[i].valid = false;
      }

      // Selectively squash instructions after the branch, in the Schedule through Writeback Stages.

      // Schedule Stage:
      IQ.squash(branch_ID);

      for (i = 0; i < issue_width; i++) {
         // Register Read Stage:
         if (Execution_Lanes[i].rr.valid && BIT_IS_ONE(Execution_Lanes[i].rr.branch_mask, branch_ID)) {
            Execution_Lanes[i].rr.valid = false;
         }

         // Execute Stage:
         for (j = 0; j < Execution_Lanes[i].ex_depth; j++) {
            if (Execution_Lanes[i].ex[j].valid && BIT_IS_ONE(Execution_Lanes[i].ex[j].branch_mask, branch_ID)) {
               Execution_Lanes[i].ex[j].valid = false;
            }
         }

         // Writeback Stage:
         if (Execution_Lanes[i].wb.valid && BIT_IS_ONE(Execution_Lanes[i].wb.branch_mask, branch_ID)) {
            Execution_Lanes[i].wb.valid = false;
         }
      }

      // VP BEGIN – partial VPQ rollback on branch misprediction
      // The branch's checkpoint saved the VPQ tail at checkpoint time.
      // Walk VPQ from that tail to current tail, decrementing instance
      // counters for each PC found, then restore VPQ tail to checkpoint.
      // Store the checkpoint VPQ tail in the
      // branch checkpoint structure in pipeline_t
      // Here we use vp_branch_vpq_tail[branch_ID]
      if (VPU.enabled) {
         VPU.partial_rollback(
	  vp_branch_vpq_tail[branch_ID],
	  vp_branch_vpq_tail_phase[branch_ID]
	);
      }
      // VP END
   }
}
