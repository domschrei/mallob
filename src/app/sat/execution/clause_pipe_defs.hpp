
#pragma once

// -> buffer limit
// <- clauses; # lits; successful solver ID
#define CLAUSE_PIPE_PREPARE_CLAUSES 'p'

// -> InPlaceClauseAggregation; epoch
// <- filter; epoch
#define CLAUSE_PIPE_FILTER_IMPORT 'f'

// -> filter; epoch
// <- # admitted lits
#define CLAUSE_PIPE_DIGEST_IMPORT 'd'

// -> InPlaceClauseAggregation; epoch
#define CLAUSE_PIPE_DIGEST_IMPORT_WITHOUT_FILTER 'D'

// -> clauses; revision
#define CLAUSE_PIPE_RETURN_CLAUSES 'r'

// -> clauses; epoch start; epoch end; revision
#define CLAUSE_PIPE_DIGEST_HISTORIC 'h'

// -> {}
#define CLAUSE_PIPE_DUMP_STATS 's'

// -> {}
#define CLAUSE_PIPE_REDUCE_THREAD_COUNT 'X'

// -> long long
#define CLAUSE_PIPE_UPDATE_BEST_FOUND_OBJECTIVE_COST 'u'

#define CLAUSE_PIPE_SET_THREAD_COUNT 'T'

#define CLAUSE_PIPE_START_NEXT_REVISION 'v'

#define CLAUSE_PIPE_SOLUTION 'S'

