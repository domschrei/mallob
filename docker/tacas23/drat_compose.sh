#!/bin/bash

# To produce the DRAT proof for getting a sense of the speed of Norbert's work, assuming the same files as before:
# 1. compose-proofs --write-unpruned drat.lrat preprocesed.cnf p.lrat <proofs>
# 2. dratify preprocessed.cnf drat.lrat drat.drat
# 3. dratify original.cnf preprocessed.lrat preprocessed.drat
# 4. cat preprocessed.drat drat.drat > final.drat
# The first step produces the combined file with deletes added but no clauses removed in pruning (in addition to the fully pruned file).  
# I added this to the proof composition because it relies on having the combined file, which we are only guaranteed during the composition and would otherwise need to reconstruct again.
# Note that step 5a of your parallel workflow is thus part of step 4a, so the split for running them in parallel needs to occur at 4b/5b, or 5a can duplicate the work done for 4a and run the whole thing in parallel.  
# The second step turns the unpruned LRAT proof into a DRAT proof.  The third step turns the preprocessing LRAT proof into a DRAT proof.  The last step combines the two.  If the proofs are binary, you need to add the --binary flag in steps 1, 2, and 3 and add step 2.5 to compress preprocessed.lrat before turning it into DRAT. 

# 1 is already done as a part of proof_compose.sh
# 2
/dratify --binary /logs/processes/input_units_removed.cnf /logs/processes/drat.{lrat,drat}
# 2.5 is done in proof_compose.sh
# 3
/dratify --binary $1 /logs/preprocessor/preprocessing-compressed.lrat /logs/preprocessor/preprocessing.drat
# 4
cat /logs/preprocessor/preprocessing.drat /logs/processes/drat.drat > /logs/processes/final.drat
