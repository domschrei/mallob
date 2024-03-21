
#pragma once

// Initialize and begin the loading stage.
// IN: #vars (int); 128-bit signature of the formula
// OUT: OK
#define TRUSTED_CHK_INIT 'B'

// Load a chunk of the original problem formula.
// IN: size integer k; sequence of k literals (0 = separator).
// OUT: (none)
#define TRUSTED_CHK_LOAD 'L'

// End the loading stage; verify the signature.
// OUT: OK
#define TRUSTED_CHK_END_LOAD 'E'

// Add the derivation of a new, local clause.
// IN: total size (#ints) k; 64-bit ID; zero-terminated lits; 64-bit hints.
// OUT: OK; 128-bit signature
#define TRUSTED_CHK_CLS_PRODUCE 'a'

// Import a clause from another solver.
// IN: total size (#ints) k; 64-bit ID; zero-terminated lits; 128-bit signature.
// OUT: OK
#define TRUSTED_CHK_CLS_IMPORT 'i'

// Delete a sequence of clauses.
// IN: total size (#ints) k; 64-bit IDs.
// OUT: OK
#define TRUSTED_CHK_CLS_DELETE 'd'

// Confirm that the formula is proven unsatisfiable.
// OUT: OK
#define TRUSTED_CHK_VALIDATE 'V'

#define TRUSTED_CHK_VALIDATE_UNSAT 'V'
#define TRUSTED_CHK_VALIDATE_SAT 'M'

#define TRUSTED_CHK_TERMINATE 'T'

#define TRUSTED_CHK_RES_ACCEPT 'A'
#define TRUSTED_CHK_RES_ERROR 'E'

#define TRUSTED_CHK_MAX_BUF_SIZE (1<<14)
