
#pragma once

// This file specifies the input and output format of the checker process.
// For all multi-byte objects such as 32- or 64-bit integers, note the
// system's endianness.

// Initialize and begin the loading stage OF A CERTAIN INCREMENT.
// IN: 128-bit signature of the formula (from trusted parser)
// OUT: OK
#define TRUSTED_CHK_BEGIN_LOAD 'B'

// Load a chunk of the original problem formula.
// IN: int k; sequence of k literals.
// OUT: (void)
#define TRUSTED_CHK_LOAD 'L'

// End the loading stage; verify the signature.
// IN: int k; sequence of k assumption literals
// OUT: OK
#define TRUSTED_CHK_END_LOAD 'E'

// Add the derivation of a new, local clause.
// IN: 64-bit ID; int k; sequence of k literals;
//     int l; sequence of l 64-bit hints;
//     "share" char (0|1) indicating whether to return a signature.
// OUT: OK; 128-bit signature only if "share"
#define TRUSTED_CHK_CLS_PRODUCE 'a'

// Import a clause from another solver.
// IN: 64-bit ID; int k; sequence of k literals; 128-bit signature; int rev.
// OUT: OK
#define TRUSTED_CHK_CLS_IMPORT 'i'

// Delete a sequence of clauses.
// IN: int k; sequence of k 64-bit IDs.
// OUT: OK
#define TRUSTED_CHK_CLS_DELETE 'd'

// Confirm that the formula is proven unsatisfiable.
// IN: 64-bit ID; int k; sequence of k failed assumption literals (falsified by the referenced clause)
// OUT: OK
#define TRUSTED_CHK_VALIDATE_UNSAT 'V'

// Check the provided model to confirm that the formula is satisfiable.
// IN: int k, sequence M of k literals where M[x] ∈ ±(x+1) indicates
//     the assignment to variable x+1.
// OUT: OK
#define TRUSTED_CHK_VALIDATE_SAT 'M'

// Terminate.
// IN: (none)
// OUT: OK
#define TRUSTED_CHK_TERMINATE 'T'

// Checker answer that everything is OK
#define TRUSTED_CHK_RES_ACCEPT 'A'
// Checker answer that an error occurred
#define TRUSTED_CHK_RES_ERROR 'E'

#define TRUSTED_CHK_MAX_BUF_SIZE (1<<14)
