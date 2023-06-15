/* Copyright (C) 2010, Armin Biere, JKU Linz. */

#include <assert.h>
#include <ctype.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <unistd.h>
#include <signal.h>

extern const char *blqr_id (void);
extern const char *blqr_version (void);
extern const char *blqr_cflags (void);

#define COVER(COVER_CONDITION) \
do { \
  if (!(COVER_CONDITION)) break; \
  fprintf (stderr, \
           "*BLOQQER* covered line %d: %s\n", \
	   __LINE__, # COVER_CONDITION); \
  if (getenv("NCOVER")) break; \
  abort (); \
} while (0)

#ifdef NLOG
#define LOG(...) do { } while (0)
#define LOGCLAUSE(...) do { } while (0)
#else
static int loglevel;
#define LOGPREFIX "c [BLOQQER] "
#define LOG(FMT,ARGS...) \
  do { \
    if (loglevel <= 0) break; \
    fputs (LOGPREFIX, stdout); \
    fprintf (stdout, FMT, ##ARGS); \
    fputc ('\n', stdout); \
    fflush (stdout); \
  } while (0)
#define LOGCLAUSE(CLAUSE,FMT,ARGS...) \
  do { \
    Node * LOGCLAUSE_P; \
    if (loglevel <= 0) break; \
    fputs (LOGPREFIX, stdout); \
    fprintf (stdout, FMT, ##ARGS); \
    for (LOGCLAUSE_P = (CLAUSE)->nodes; LOGCLAUSE_P->lit; LOGCLAUSE_P++) \
      fprintf (stdout, " %d", LOGCLAUSE_P->lit); \
    fputc ('\n', stdout); \
    fflush (stdout); \
  } while (0)
#endif

#define INC(B) \
  do { \
    current_bytes += B; \
    if (max_bytes < current_bytes) \
      max_bytes = current_bytes; \
  } while (0)
#define DEC(B) \
  do { \
    assert (current_bytes >= B); \
    current_bytes -= B; \
  } while (0)
#define NEWN(P,N) \
  do { \
    size_t NEWN_BYTES = (N) * sizeof *(P); \
    (P) = malloc (NEWN_BYTES); \
    if (!(P)) die ("out of memory"); \
    memset ((P), 0, NEWN_BYTES); \
    INC (NEWN_BYTES); \
  } while (0)
#define DELN(P,N) \
  do { \
    size_t DELN_BYTES = (N) * sizeof *(P); \
    DEC (DELN_BYTES); \
    free (P); \
  } while (0)
#define RSZ(P,M,N) \
  do { \
    size_t RSZ_OLD_BYTES = (M) * sizeof *(P); \
    size_t RSZ_NEW_BYTES = (N) * sizeof *(P); \
    DEC (RSZ_OLD_BYTES); \
    (P) = realloc (P, RSZ_NEW_BYTES); \
    if (RSZ_OLD_BYTES > 0 && !(P)) die ("out of memory"); \
    if (RSZ_NEW_BYTES > RSZ_OLD_BYTES) { \
      size_t RSZ_INC_BYTES = RSZ_NEW_BYTES - RSZ_OLD_BYTES; \
      memset (RSZ_OLD_BYTES + ((char*)(P)), 0, RSZ_INC_BYTES); \
    } \
    INC (RSZ_NEW_BYTES); \
  } while (0)
#define NEW(P) NEWN((P),1)
#define DEL(P) DELN((P),1)

typedef unsigned long long Sig;

typedef struct Occ {		/* occurrence list anchor */
  int count;
  struct Node * first, * last;
} Occ;

typedef enum Tag {
  FREE = 0,
  UNET = 1,
  UNATE = 2,
  FIXED = 3,
  ZOMBIE = 4,
  ELIMINATED = 5,
  SUBSTITUTED = 6,
  EXPANDED = 7,
} Tag;

typedef struct Var {
  struct Scope * scope;
  Tag tag;
  int mark;			/* primary mark flag */
  int submark;			/* second subsumption mark flag */
  int mapped;			/* index mapped to */
  int fixed;			/* assignment */
  int score, pos;		/* for elimination priority queue */
  int expcopy;			/* copy in expansion */
  struct Var * prev, * next;	/* scope variable list links */
  Occ occs[2];			/* positive and negative occurrence lists */
} Var;

typedef struct Scope {
  int type;			/* type>0 == existential, type<0 universal */
  int order;			/* scope order: outer most = 0 */
  int free;			/* remaining free variables */
  int stretch;			/* stretches to this scope */
  struct Var * first, * last;	/* variable list */
  struct Scope * outer, * inner;
} Scope;

typedef struct Node {		/* one 'lit' occurrence in a 'clause' */
  int lit;
  struct Clause * clause;
  struct Node * prev, * next;	/* links all occurrences of 'lit' */
} Node;

typedef struct Anchor {		/* list anchor for forward watches */
  int count;
  struct Clause * first, * last;
} Anchor;

typedef struct Watch {		/* watch for forward subsumption */
  int idx;			/* watched 'idx' */
  struct Clause * prev, * next;	/* links for watches of 'idx' */
} Watch;

typedef struct Clause {
  int size;
  int mark;
  Sig sig;			/* subsumption/strengthening signature */
  struct Clause * prev, * next;	/* chronlogical clause list links */
  struct Clause * head, * tail;	/* backward subsumption queue links */
  Watch watch;			/* forward subsumption watch */
  Node nodes[1];		/* embedded literal nodes */
} Clause;

typedef struct Opt {
  char inc;
  const char * name;
  int def, low, high;
  const char * description;
  int * valptr;
} Opt;

static int verbose, bce, eq, ve, quantifyall, force, strict, keep, output;
static int help, range, defaults, bound, hte, htesize, hteoccs, htesteps;
static int embedded, ignore, cce, hbce, exp, axcess, splitlim;
static int fwmaxoccs, fwmax1size, fwmax2size;
static int bwmaxoccs, bwmax1size, bwmax2size;
static int blkmax1occs, blkmax2occs;
static int blkmax1size, blkmax2size;
static int elimoccs, elimsize, excess;

static int expand_variable;
static int maxexpvarcost;

#define IM INT_MAX

static Opt opts[] = {
{'h',"help",0,0,1,"command line options summary",&help},
#ifndef NLOG
{'l',"log",0,0,1,"set logging level",&loglevel},
#endif
{'v',"verbose",0,0,2,"set verbosity level",&verbose},
{'i',"ignore",0,0,1,"ignore embedded options",&ignore},
{000,"output",1,0,1,"do not print preprocessed CNF",&output},
{'d',"defaults",0,0,1,"print command line option default values",&defaults},
{'e',"embedded",0,0,1,"as '-d' but in embedded format",&embedded},
{'r',"range",0,0,1,"print comand line option value ranges",&range},
{'k',"keep",1,0,1,"keep original variable indices",&keep},
{'f',"force",0,0,1,"force reading if clause number incorrect",&force},
{'q',"quantify-all",1,0,1,"quantify all variables in output",&quantifyall},
{000,"split",512,3,IM,"split long clauses of at least this length",&splitlim},
{000,"bce",1,0,1,"enable blocked clause elimination",&bce},
{000,"eq",1,0,1,"enable equivalent literal reasoning",&eq},
{000,"ve",1,0,1,"enable variable elimination",&ve},
{000,"exp",1,0,1,"enable variable expansion",&exp},
{000,"hte",1,0,1,"enable hidden clause elimination",&hte},
{000,"cce",1,0,1,"enable covered literal addition",&cce},
{000,"hbce",1,0,1,"enable hidden blocked clause elimination",&hbce},
{'s',"strict",0,0,1,"enforce strict variable elimination",&strict},
{000,"excess",20,-IM,IM,"excess limit in variable elimination",&excess},
{000,"axcess",2000,-IM,IM,"excess limit in variable expansion",&axcess},
{000,"fwmaxoccs",32,0,IM,"forward max occurrences",&fwmaxoccs},
{000,"fwmax1size",32,0,IM,"inner forward max clause size", &fwmax1size},
{000,"fwmax2size",64,0,IM,"outer forward max clause size", &fwmax2size},
{000,"bwmaxoccs",16,0,IM,"backward max occurrences",&bwmaxoccs},
{000,"bwmax1size",16,0,IM,"inner backward max clause size", &bwmax1size},
{000,"bwmax2size",32,0,IM,"outer backward max clause size", &fwmax2size},
{000,"blkmax1occs",32,0,IM,"inner max blocking lit occs",&blkmax1occs},
{000,"blkmax2occs",16,0,IM,"outer max blocking lit occs",&blkmax2occs},
{000,"blkmax1size",32,0,IM,"inner max blocked clause size",&blkmax1size},
{000,"blkmax2size",16,0,IM,"outer max blocked clause size",&blkmax2size},
{000,"elimoccs",32,0,IM,"max eliminated variable occs",&elimoccs},
{000,"elimsize",32,0,IM,"max eliminated clauses size",&elimsize},
{000,"bound",1024,-1,IM,"bound for all bw/fw/block/elim limits",&bound},
{000,"htesteps",64,0,IM,"hte steps bound",&htesteps},
{000,"hteoccs",32,0,IM,"hte max occurrences size",&hteoccs},
{000,"htesize",1024,2,IM,"hte max clause size",&htesize},

// NEW ADDITIONS:
{000,"maxexpvarcost",10000,0,IM,"maximum cost of expanding the variable",&maxexpvarcost},
{000,"expvar",0,0,IM,"expand one variable",&expand_variable},

{000,0},
};

static void apply_expansion_config() {
  bound = IM;
  //axcess = IM;
}

static const char * iname, * oname;
static FILE * ifile, * ofile;
static int remaining_clauses_to_parse;
static int ifclose, ipclose, oclose;
static int lineno = 1;

static int terminal, notclean;
static struct itimerval timer, old_timer;
static const char * timerstr;
static int * timerptr;
static void (*old_alarm_handler)(int);

static Scope * inner_most_scope, * outer_most_scope;
static Clause * first_clause, * last_clause, * empty_clause, * queue;
static int nqueue;

static int num_vars, fixed, elimidx, substituting;
static int universal_vars, existential_vars, implicit_vars;
static int * schedule, size_schedule;
static int * trail, * top_of_trail, * next_on_trail;
static int * dfsi, * mindfsi, * repr;
static Sig * fwsigs, * bwsigs;
static Anchor * anchors;
static Var * vars;

static int nstack, szstack, * stack;
static int num_lits, size_lits, * lits;
static int naux, szaux, * aux;
static int nline, szline;
static char * line;

static size_t current_bytes, max_bytes;
static int embedded_options, command_line_options;
static int added_clauses, added_binary_clauses, enqueued_clauses, pushed_vars;
static int subsumed_clauses, strengthened_clauses;
static int forward_subsumed_clauses, forward_strengthened_clauses;
static int backward_subsumed_clauses, backward_strengthened_clauses;
static int blocked_clauses, orig_clauses, num_clauses, hidden_tautologies;
static int units, unates, unets, zombies, eliminated, remaining, mapped;
static int substituted, eqrounds, nonstrictves, hidden_blocked_clauses;
static int added_binary_clauses_at_last_eqround;
static int expanded, expansion_cost_mark;
static struct { struct { int64_t lookups, hits; } sig1, sig2; } fw, bw;
static long long hlas, clas;

static int print_progress (void) { 
#ifndef NLOG
  if (loglevel) return 0;
#endif
  if (verbose < 2) return 0;
  return terminal;
}

static void progress_handler (int s) {
  char buffer[80];
  int len;
  assert (s == SIGALRM);
  assert (timerstr);
  if (!timerstr || !timerptr) return;
  sprintf (buffer, "c [bloqqer] %s %d", timerstr, *timerptr);
  notclean = 1;
  fputs (buffer, stdout);
  len = strlen (buffer);
  while (len++ < 70) putc (' ', stdout);
  putc ('\r', stdout);
  fflush (stdout);
}

static void start_progress (const char * msg, int * ptr) {
  if (!print_progress ()) return;
  assert (msg);
  assert (ptr);
  timer.it_interval.tv_sec = 0;
  timer.it_interval.tv_usec = 10000;
  timer.it_value = timer.it_interval;
  old_alarm_handler = signal (SIGALRM, progress_handler);
  setitimer (ITIMER_REAL, &timer, &old_timer);
  assert (!timerstr);
  assert (!timerptr);
  timerstr = msg;
  timerptr = ptr;
}

static void clr_progress_handler () {
  signal (SIGALRM, old_alarm_handler);
  assert (timerstr);
  timerstr = 0;
  timerptr = 0;
  setitimer (ITIMER_REAL, &old_timer, 0);
}

static void clean_line (void) {
  int i;
  if (!notclean) return;
  fputc ('\r', stdout);
  for (i = 0; i < 70; i++) fputc (' ', stdout);
  fputc ('\r', stdout);
  fflush (stdout);
  notclean = 0;
}

static void stop_progress (void) {
  if (!print_progress ()) return;
  clr_progress_handler ();
  clean_line ();
}

static void die (const char * fmt, ...) {
  va_list ap;
  if (timerstr) stop_progress ();
  fputs ("*** bloqqer: ", stderr);
  va_start (ap, fmt);
  vfprintf (stderr, fmt, ap);
  va_end (ap);
  fputc ('\n', stderr);
  fflush (stderr);
  exit (1);
}

static void msg (const char * fmt, ...) {
  va_list ap;
  if (!verbose) return;
  if (notclean) clean_line ();
  fputs ("c [bloqqer] ", stdout);
  va_start (ap, fmt);
  vfprintf (stdout, fmt, ap);
  va_end (ap);
  fputc ('\n', stdout);
  fflush (stdout);
}

static void wrn (const char * fmt, ...) {
  va_list ap;
  if (notclean) clean_line ();
  fputs ("c *bloqqer* ", stdout);
  va_start (ap, fmt);
  vfprintf (stdout, fmt, ap);
  va_end (ap);
  fputc ('\n', stdout);
  fflush (stdout);
}

static double seconds (void) {
  struct rusage u;
  double res;
  if (getrusage (RUSAGE_SELF, &u)) return 0;
  res = u.ru_utime.tv_sec + 1e-6 * u.ru_utime.tv_usec;
  res += u.ru_stime.tv_sec + 1e-6 * u.ru_stime.tv_usec;
  return res;
}

static Var * lit2var (int lit) {
  assert (lit && abs (lit) <= num_vars);
  return vars + abs (lit);
}

static Scope * lit2scope (int lit) {
  return lit2var (lit)->scope;
}

static int sign (int lit) {
  return lit < 0 ? -1 : 1;
}

#ifndef NLOG
static const char * type2str (int type) {
  return type < 0 ? "universal" : "existential";
}
#endif

static void add_outer_most_scope (void) {
  NEW (outer_most_scope);
  outer_most_scope->type = 1;
  inner_most_scope = outer_most_scope;
  LOG ("new outer most existential scope 0");
}

static void add_var (int idx, Scope * scope) {
  Var * v;
  assert (0 < idx && idx <= num_vars);
  v = lit2var (idx);
  assert (!v->scope);
  v->scope = scope;
  v->prev = scope->last;
  if (scope->last) scope->last->next = v;
  else scope->first = v;
  scope->last = v;
  LOG ("adding %s variable %d to %s scope %d",
       type2str (scope->type), idx, type2str (scope->type), scope->order);
  assert (v->tag == FREE);
  scope->free++;
}

static void add_quantifier (int lit) {
  Scope * scope;
  if (!outer_most_scope) add_outer_most_scope ();
  if (inner_most_scope->type != sign (lit)) {
    NEW (scope);
    scope->outer = inner_most_scope;
    scope->type = sign (lit);
    scope->order = inner_most_scope->order + 1;
    scope->stretch = scope->order;
    inner_most_scope->inner = scope;
    inner_most_scope = scope;
    LOG ("new inner %s scope %d", type2str (scope->type), scope->order);
  } else scope = inner_most_scope;
  add_var (abs (lit), scope);
}

static size_t bytes_clause (int size) {
  return sizeof (Clause) + size * sizeof (Node);
}

static int existential (int lit) {
  return lit2scope (lit)->type > 0;
}

static int universal (int lit) {
  return lit2scope (lit)->type < 0;
}

static int lit2order (int lit) {
  return lit2scope (lit)->order;
}

static int deref (int lit) {
  Var * v = lit2var (lit);
  int res = v->fixed;
  if (lit < 0) res = -res;
  return res;
}

static Occ * lit2occ (int lit) {
  Var * v = lit2var (lit);
  return v->occs + (lit < 0);
}

static void forall_reduce_clause (void) {
  int i, j, lit, order, tmp;
  order = 0;
  for (i = 0; i < num_lits; i++) {
    lit = lits[i];
    assert (!deref (lit));
    if (universal (lit)) continue;
    tmp = lit2order (lit);
    if (tmp <= order) continue;
    order = tmp;
  }
  j = 0;
  for (i = 0; i < num_lits; i++) {
    lit = lits[i];
    if (existential (lit) || lit2order (lit) < order) lits[j++] = lit;
  }
  num_lits = j;
}

static int trivial_clause (void) {
  int i, j, trivial, lit, tmp;
  Var * v;
  trivial = j = 0;
  for (i = 0; !trivial && i < num_lits; i++) {
    lit = lits[i];
    tmp = deref (lit);
    if (tmp > 0) trivial = 1;
    else if (!tmp) {
      v = lit2var (lit);
      if (v->mark == -lit) trivial = 1;
      else if (!v->mark) {
	lits[j++] = lit;
	v->mark = lit;
      }
    }
  }
  num_lits = j;
  for (i = 0; i < num_lits; i++)
    lit2var (lits[i])->mark = 0;
  return trivial;
}

static void tag_var (Var * v, Tag tag) {
  Scope * scope;
  assert (v->tag == FREE);
  assert (tag != FREE);
  v->tag = tag;
  scope = v->scope;
  assert (scope->free > 0);
  scope->free--;
#ifndef NLOG
  if (!scope->free) LOG ("scope %d becomes empty", scope->order);
#endif
}

static void tag_lit (int lit, Tag tag) { tag_var (lit2var (lit), tag); }

static void assign (int lit) {
  Var * v;
  assert (!deref (lit));
  assert (top_of_trail < trail + num_vars);
  *top_of_trail++ = lit;
  v = lit2var (lit);
  v->fixed = lit;
  LOG ("assign %d", lit);
  if (v->tag != FREE) return;
  fixed++;
  tag_var (v, FIXED);
  assert (remaining > 0);
  remaining--;
  LOG ("fixed %d remaining %d", lit, remaining);
}

static int isfree (int lit) {
  return lit2var (lit)->tag == FREE;
}

static void zombie (int lit) {
  assert (!lit2occ (lit)->count);
  assert (!lit2occ (-lit)->count);
  if (substituting) return;
  if (abs (lit) == elimidx) return;
  if (!isfree (lit)) return;
  if (deref (lit)) return;
  zombies++;
  tag_lit (lit, ZOMBIE);
  assert (remaining > 0);
  remaining--;
  LOG ("new zombie %d remaining %d", lit, remaining);
}

static void unet (int lit) {
  assert (existential (lit));
  assert (!lit2occ (-lit)->count);
  if (substituting) return;
  if (abs (lit) == elimidx) return;
  if (!isfree (lit)) return;
  if (deref (lit)) return;
  unets++;
  tag_lit (lit, UNET);
  assert (remaining > 0);
  remaining--;
  LOG ("existential pure literal %d remaining %d", lit, remaining);
  assign (lit);
}

static void unate (int lit) {
  assert (universal (lit));
  assert (!lit2occ (-lit)->count);
  assert (abs (lit) != elimidx);
  if (substituting) return;
  if (!isfree (lit)) return;
  if (deref (lit)) return;
  unates++;
  tag_lit (lit, UNATE);
  assert (remaining > 0);
  remaining--;
  LOG ("universal pure literal %d remaining %d", lit, remaining);
  assign (-lit);
}

static void check_schedule (void) {
#ifndef NDEBUG
#if 0
#warning "expensive schedule checking code enabled"
  int ppos, cpos, parent, child, i;
  Var * pvar, * cvar;
  for (ppos = 0; ppos < size_schedule; ppos++) {
    parent = schedule[ppos];
    pvar = lit2var (parent);
    assert (ppos == pvar->pos);
    for (i = 0; i <= 1; i++) {
      cpos = 2*ppos + 1 + i;
      if (cpos >= size_schedule) continue;
      child = schedule[cpos];
      cvar = lit2var (child);
      assert (cvar->pos == cpos);
      assert (pvar->score <= cvar->score);
    }
  }
#endif
#endif
}

static void up (int idx) {
  int child = idx, parent, cpos, ppos, cscore;
  Var * cvar = lit2var (child), * pvar;
  cscore = cvar->score;
  cpos = cvar->pos;
  assert (cpos >= 0);
  assert (schedule[cpos] == child);
  while (cpos > 0) {
    ppos = (cpos - 1)/2;
    parent = schedule[ppos];
    pvar = lit2var (parent);
    assert (pvar->pos == ppos);
    if (pvar->score <= cscore) break;
    schedule[cpos] = parent;
    pvar->pos = cpos;
    LOG ("down %d with score %d to %d from %d",
         parent, pvar->score, cpos, ppos);
    cpos = ppos;
    child = parent;
  }
  LOG ("up %d with score %d to %d from %d", idx, cscore, cpos, cvar->pos);
  schedule[cpos] = idx;
  cvar->pos = cpos;
  check_schedule ();
}

static void down (int idx) {
  int parent = idx, child, right, ppos, cpos, pscore;
  Var * pvar = lit2var (parent), * cvar, * rvar;
  pscore = pvar->score;
  ppos = pvar->pos;
  assert (0 <= ppos && ppos < size_schedule);
  assert (schedule[ppos] == parent);
  for (;;) {
    cpos = 2*ppos + 1;
    if (cpos >= size_schedule) break;
    child = schedule[cpos];
    cvar = lit2var (child);
    if (cpos + 1 < size_schedule) {
      right = schedule[cpos + 1];
      rvar = lit2var (right);
      if (cvar->score > rvar->score)
	cpos++, child = right, cvar = rvar;
    }
    if (cvar->score >= pscore) break;
    assert (cvar->pos == cpos);
    schedule[ppos] = child;
    cvar->pos = ppos;
    LOG ("up %d with score %d to %d from %d", child, cvar->score, ppos, cpos);
    ppos = cpos;
    parent = child;
  }
  LOG ("down %d with score %d to %d from %d", idx, pscore, ppos, pvar->pos);
  schedule[ppos] = idx;
  pvar->pos = ppos;
  check_schedule ();
}

static void push_schedule (int idx) {
  Var * v = lit2var (idx);
  assert (v->pos < 0);
  assert (size_schedule < num_vars);
  schedule[v->pos = size_schedule++] = idx;
  LOG ("push %d", idx);
  up (idx);
  pushed_vars++;
}

static void update_score (int idx) {
  Var * v = lit2var (idx);
  int old_score = v->score;
  int pos = v->occs[0].count;
  int neg = v->occs[1].count;
  int new_score = pos + neg;
  assert (idx > 0);
  v->score = new_score;
  if (new_score < old_score)
    LOG ("score of %d decreased to %d", idx, new_score);
  if (new_score > old_score)
    LOG ("score of %d increased to %d", idx, new_score);
  if (v->pos < 0) {
     if (deref (idx)) return;
     if (elimidx != idx) push_schedule (idx);
  }
  if (new_score < old_score) {
    if (!empty_clause) {
      if (pos && !neg) {
	if (existential (idx)) unet (idx); else unate (idx);
      } else if (neg && !pos) {
	if (existential (idx)) unet (-idx); else unate (-idx);
      }
    }
    if (v->pos >= 0) up (idx);
  }
  if (new_score > old_score && v->pos >= 0) down (idx);
}

static void add_node (Clause * clause, Node * node, int lit) {
  Occ * occ;
  assert (clause->nodes <= node && node < clause->nodes + clause->size);
  node->clause = clause;
  node->lit = lit;
  occ = lit2occ (lit);
  node->prev = occ->last;
  if (occ->last) occ->last->next = node;
  else occ->first = node;
  occ->last = node;
  occ->count++;
  LOG ("number of occurrences of %d increased to %d", lit, occ->count);
  update_score (abs (lit));
}

static int enqueued (Clause * clause) {
  assert (!clause->head == !clause->tail);
  return clause->head != 0;
}

static void check_queue (void) {
#if 0
#ifndef NDEBUG
#warning "expensive queue checking code enabled"
  Clause * head, * tail, * prev, * clause;
  int count;
  if (!queue) { assert (!nqueue); return; }
  clause = head = queue;
  tail = head->head;
  prev = tail;
  count = 0;
  do {
    count++;
    assert (clause->head == prev);
    assert (prev->tail == clause);
    prev = clause;
    clause = clause->tail;
  } while (clause != head);
  assert (count == nqueue);
#endif
#endif
}

static void enqueue (Clause * clause) {
  Clause * head, * tail;
  enqueued_clauses++;
  assert (!enqueued (clause));
  assert (!clause->head && !clause->tail);
  if (queue) {
    head = queue;
    tail = head->head;
    tail->tail = head->head = clause;
    clause->tail = head;
    clause->head = tail;
  } else queue = clause->head = clause->tail = clause;
  nqueue++;
  LOGCLAUSE (clause, "enqueued clause");
  assert (enqueued (clause));
  check_queue ();
}

static Sig lit2sig (int lit) {
  return 1ull << ((100623947llu * (Sig) abs (lit)) & 63llu);
}

static int least_occurring_idx (Clause * clause) {
  int best = INT_MAX, tmp, start = 0, lit;
  Node * p;
  assert (clause->size > 0);
  for (p = clause->nodes; (lit = p->lit); p++) {
    tmp = lit2occ (lit)->count + lit2occ (-lit)->count;
    if (best <= tmp) continue;
    start = abs (lit);
    best = tmp;
  }
  assert (start);
  assert (best < INT_MAX);
  LOG ("variable %d has shortest occurrence list of length %d", start, best);
  assert (best >= 1);
  return start;
}

static void watch_clause (Clause * clause) {
  int watched, lit;
  Anchor * anchor;
  Node * p;
  Sig sig;
  assert (clause->size > 1);
  watched = least_occurring_idx (clause);
  anchor = anchors + abs (watched);
  LOG ("watching %d", abs (watched));
  clause->watch.idx = abs (watched);
  if (anchor->last) {
    assert (anchor->first);
    assert (anchor->last->watch.idx == abs (watched));
    anchor->last->watch.next = clause;
  } else {
    assert (!anchor->first);
    anchor->first = clause;
  }
  clause->watch.prev = anchor->last;
  anchor->last = clause;
  anchor->count++;
  assert (anchor->count >= 0);
  sig = 0llu;
  for (p = clause->nodes; (lit = p->lit); p++)
    if (abs (lit) != watched) sig |= lit2sig (lit);
  LOG ("forward sig  %016llx without %d", sig, watched);
  fwsigs [ watched ] |= sig;
}

static int forward_subsumed_by_clause (Clause * clause, Sig sig) {
  Node * p;
  int lit;
  Var * v;
  if (num_lits < clause->size) return 0;
  fw.sig1.lookups++;
  if (clause->sig & ~sig) { fw.sig1.hits++; return 0; }
  for (p = clause->nodes; (lit = p->lit); p++) {
    v = lit2var (lit);
    if (v->mark != lit) return 0;
  }
  return 1;
}

static void check_all_unmarked (void) {
#if 0
#warning "expensive checking that all 'mark' flags are clear enabled"
  int idx;
  for (idx = 1; idx <= num_vars; idx++)
    assert (!vars[idx].mark);
#endif
}

static void mark_lit (int lit) {
  Var * v = lit2var (lit);
  assert (!v->mark);
  v->mark = lit;
}

static void submark_lit (int lit) {
  Var * v = lit2var (lit);
  assert (!v->submark);
  v->submark = lit;
}

static void unsubmark_lit (int lit) {
  Var * v = lit2var (lit);
  assert (v->submark == lit);
  v->submark = 0;
}

static void mark_lits (void) {
  int i;
  check_all_unmarked ();
  for (i = 0; i < num_lits; i++)
    mark_lit (lits[i]);
}

static void unmark_lit (int lit) {
  Var * v = lit2var (lit);
  assert (v->mark == lit);
  v->mark = 0;
}

static void unmark_lits (void) {
  int i;
  for (i = 0; i < num_lits; i++)
    unmark_lit (lits[i]);
  check_all_unmarked ();
}

static Sig sig_lits (void) {
  Sig res = 0ull;
  int i, lit;
  for (i = 0; i < num_lits; i++) {
    lit = lits[i];
    res |= lit2sig (lit);
  }
  return res;
}

static int forward_subsumed (void) {
  int i, res, lit;
  Anchor * anchor;
  Clause * p;
  Sig sig;
  res = 0;
  mark_lits ();
  sig = sig_lits ();
  for (i = 0; !res && i < num_lits; i++) {
    lit = lits[i];
    fw.sig2.lookups++;
    if (!(fwsigs [ abs (lit) ] & sig)) { fw.sig2.hits++; continue; }
    anchor = anchors + abs (lit);
    if (anchor->count > fwmaxoccs) continue;
    for (p = anchor->first; p; p = p->watch.next)
      if ((res = forward_subsumed_by_clause (p, sig))) break;
  }
  unmark_lits ();
  if (res) {
#ifndef NLOG
    if (loglevel) {
      fputs (LOGPREFIX, stdout);
      fputs ("forward subsumed clause", stdout);
      for (i = 0; i < num_lits; i++) printf (" %d", lits[i]);
      fputc ('\n', stdout);
      fflush (stdout);
      LOGCLAUSE (p, "forward subsuming clause");
    }
#endif
    forward_subsumed_clauses++;
    subsumed_clauses++;
    num_lits = 0;
  }
  return res;
}

static int forward_strengthened_by_clause (Clause * clause, Sig sig) {
  int lit, tmp, res;
  Node * p;
  Var * v;
  assert (num_lits <= fwmax2size);
  if (clause->size > fwmax1size) return 0;
  if (num_lits < clause->size) return 0;
  fw.sig1.lookups++;
  if (clause->sig & ~sig) { fw.sig1.hits++; return 0; }
  res = 0;
  for (p = clause->nodes; (lit = p->lit); p++) {
    v = lit2var (lit);
    tmp = v->mark;
    if (tmp == lit) continue;
    if (tmp != -lit) return 0;
    if (res) return 0;
    res = -lit;
  }
  return res;
}

static void forward_strengthen (void) {
  int i, j, k, res, lit, pivot, other, found;
  Anchor * anchor;
  Clause * p;
  Sig sig;
  Var * v;
  if (num_lits > fwmax2size) return;
  sig = sig_lits ();
  mark_lits ();
  res = 0;
RESTART:
  for (i = 0; !res && i < num_lits; i++) {
    lit = lits[i];
    fw.sig2.lookups++;
    if (!(fwsigs [ abs (lit) ] & sig)) { fw.sig2.hits++; continue; }
    anchor = anchors + abs (lit);
    if (anchor->count > fwmaxoccs) continue;
    for (p = anchor->first; p; p = p->watch.next) {
      pivot = forward_strengthened_by_clause (p, sig);
      if (!pivot) continue;
#ifndef NLOG
      if (loglevel) {
	fputs (LOGPREFIX, stdout);
	printf ("forward strengthening by removing %d from clause", pivot);
	for (j = 0; j < num_lits; j++) printf (" %d", lits[j]);
	fputc ('\n', stdout);
	fflush (stdout);
	LOGCLAUSE (p, "used clause for forward strengthening");
      }
#endif
      v = lit2var (pivot);
      assert (v->mark == pivot);
      v->mark = 0;
      k = 0;
      found = 0;
      for (j = 0; j < num_lits; j++) {
	other = lits[j];
	if (other != pivot) lits[k++] = other;
	else found = 1;
      }
      assert (k + 1 == num_lits);
      assert (found);
      num_lits--;
      sig = sig_lits ();
      forward_strengthened_clauses++;
      strengthened_clauses++;
      goto RESTART;
    }
  }
  unmark_lits ();
}

static void add_clause (void) {
  Clause * clause;
  size_t bytes;
  Sig bwsig;
  int i;
  bwsig = sig_lits ();
  if (forward_subsumed ()) return;
  forward_strengthen ();
  forall_reduce_clause ();
  bytes = bytes_clause (num_lits);
  clause = malloc (bytes);
  if (!clause) die ("out of memory");
  memset (clause, 0, bytes);
  assert (!clause->nodes[num_lits].lit);
  INC (bytes);
  clause->size = num_lits;
  clause->prev = last_clause;
  clause->sig = sig_lits ();;
  LOG ("signature %016llx", clause->sig);
  if (last_clause) last_clause->next = clause;
  else first_clause = clause;
  last_clause = clause;
  for (i = 0; i < num_lits; i++)
    add_node (clause, clause->nodes + i, lits[i]);
  num_lits = 0;
  LOGCLAUSE (clause, "adding length %d clause", clause->size);
  if (!clause->size) {
    if (empty_clause) msg ("found another empty clause");
    else {
      msg ("found empty clause");
      empty_clause = clause;
    }
  } else if (clause->size == 1) {
    int lit = clause->nodes[0].lit;
    LOG ("unit %d", lit);
    units++;
    assign (lit);
  } else {
    enqueue (clause);
    watch_clause (clause);
  }
  added_clauses++;
  if (clause->size == 2) added_binary_clauses++;
  num_clauses++;
}

static void delete_node (Node * node) {
  Occ * occ = lit2occ (node->lit);
  assert (occ-> count > 0);
  if (node->prev) {
    assert (node->prev->next == node);
    node->prev->next = node->next;
  } else {
    assert (occ->first == node);
    occ->first = node->next;
  }
  if (node->next) {
    assert (node->next->prev == node);
    node->next->prev = node->prev;
  } else {
    assert (occ->last == node);
    occ->last = node->prev;
  }
  occ->count--;
  LOG ("number of occurrences of %d decreased to %d", node->lit, occ->count);
  update_score (abs (node->lit));
}

static void dequeue (Clause * clause) {
  assert (nqueue > 0);
  assert (enqueued (clause));
  if (clause->head == clause) {
    assert (clause->tail == clause);
    assert (queue == clause);
    queue = 0;
  } else {
    clause->head->tail = clause->tail;
    clause->tail->head = clause->head;
    if (queue == clause) queue = clause->tail;
  }
  clause->head = clause->tail = 0;
  LOGCLAUSE (clause, "dequeued clause");
  assert (!enqueued (clause));
  nqueue--;
}

static void unwatch_clause (Clause * clause) {
  Anchor * anchor;
  assert (clause->size > 1);
  anchor = anchors + abs (clause->watch.idx);
  if (clause->watch.prev) {
    clause->watch.prev->watch.next = clause->watch.next;
  } else {
    assert (anchor->first == clause);
    anchor->first = clause->watch.next;
  }
  if (clause->watch.next) {
    clause->watch.next->watch.prev = clause->watch.prev;
  } else {
    assert (anchor->last == clause);
    anchor->last = clause->watch.prev;
  }
  LOG ("unwatched %d", clause->watch.idx);
  assert (anchor->count > 0);
  anchor->count--;
}

static void delete_clause (Clause * clause) {
  size_t bytes;
  int i;
  assert (num_clauses > 0);
  LOGCLAUSE (clause, "deleting length %d clause", clause->size);
  bytes = bytes_clause (clause->size);
  if (clause->prev) {
    assert (clause->prev->next == clause);
    clause->prev->next = clause->next;
  } else {
    assert (first_clause == clause);
    first_clause = clause->next;
  }
  if (clause->next) {
    assert (clause->next->prev == clause);
    clause->next->prev = clause->prev;
  } else {
    assert (last_clause == clause);
    last_clause = clause->prev;
  }
  for (i = 0; i < clause->size; i++)
    delete_node (clause->nodes + i);
  if (clause->size > 1) unwatch_clause (clause);
  if (enqueued (clause)) dequeue (clause);
  DEC (bytes);
  free (clause);
  num_clauses--;
}

static void enlarge_lits (void) {
  int new_size_lits = size_lits ? 2*size_lits : 1;
  RSZ (lits, size_lits, new_size_lits);
  size_lits = new_size_lits;
}

static void push_literal (int lit) {
  assert (abs (lit) <= num_vars);
  if (size_lits == num_lits) enlarge_lits ();
  lits[num_lits++] = lit;
}

static void log_declared_scopes (void) {
  int count, o, c;
  Scope * p;
  Var * q;
  if (notclean) clean_line ();
  fputs ("c [bloqqer] ", stdout);
  for (p = outer_most_scope; p; p = p->inner) {
    if (p == outer_most_scope) assert (p->type > 0), o = '(', c = ')';
    else if (p->type < 0) o = '[', c = ']';
    else o = '<', c = '>';
    fputc (o, stdout);
    count = 0;
    for (q = p->first; q; q = q->next)
      count++;
    printf ("%d", count);
    fputc (c, stdout);
  }
  fputc ('\n', stdout);
  fflush (stdout);
}

static void init_implicit_scope (void) {
  Var * v;
  int idx;
  assert (!implicit_vars);
  implicit_vars = num_vars - universal_vars - existential_vars;
  existential_vars += implicit_vars;
  assert (implicit_vars >= 0);
  msg ("%d universal, %d existential variables (%d implicit)",
       universal_vars, existential_vars, implicit_vars);
  if (implicit_vars > 0) {
    if (!outer_most_scope) add_outer_most_scope ();
    for (v = vars + 1; v <= vars + num_vars; v++)
      if (!v->scope) {
        idx = v - vars;
        LOG ("implicitly existentially quantified variable %d", idx);
        add_var (idx, outer_most_scope);
      }
  }
  if (verbose) log_declared_scopes ();
}

static void init_variables (int start) {
  int i;
  assert (0 < start && start <= num_vars);
  for (i = start; i <= num_vars; i++) {
    Var * v = vars + i;
    v->pos = -1;
    v->mapped = ++mapped;
    assert (mapped == i);
  }
  assert (mapped == num_vars);
}

static int null_occurrences (int lit) {
  Var * v = lit2var (lit);
  if (v->occs[0].count) return 0;
  if (v->occs[1].count) return 0;
  return 1;
}

static void log_pruned_scopes (void) {
  int count, prev, this;
  Scope * p;
  Var * q;
  if (notclean) clean_line ();
  fputs ("c [bloqqer] ", stdout);
  count = this = prev = 0;
  for (p = outer_most_scope; p; p = p->inner) {
    this = 0;
    for (q = p->first; q; q = q->next)
      if (q->tag == FREE) this++;
    if (!this) continue;
    if (prev != p->type) {
      if (prev) {
	fputc ((prev < 0) ? '[' : '<', stdout);
	printf ("%d", count);
	fputc ((prev < 0) ? ']' : '>', stdout);
	count = 0;
      } else assert (!count);
      prev = p->type;
    }
    count += this;
  }
  fputc ((prev < 0) ? '[' : '<', stdout);
  printf ("%d", count);
  fputc ((prev < 0) ? ']' : '>', stdout);
  fputc ('\n', stdout);
  fflush (stdout);
}

static int parse_short_opt (const char * arg) {
  int ch = arg[0], val;
  Opt * p;
  if (!ch || arg[1]) return 0;
  for (p = opts; p->inc != ch && p->name; p++)
    ;
  if (!p->name) return 0;
  val = *p->valptr + 1;
  if (val <= p->high) *p->valptr = val;
  else wrn ("ignoring '-%c' (maximum value %d reached)", p->inc, p->high);
  return 1;
}

static void force_bound (void) {
  fwmaxoccs = fwmax1size = fwmax2size = bwmaxoccs = bound;
  bwmax1size = bwmax2size = blkmax1occs = blkmax2occs = bound;
  blkmax1size = blkmax2size = elimoccs = elimsize = bound;
}

static void reinit_opt (const char * name) {
  Opt * p;
  for (p = opts; p->name; p++)
    if (!strcmp (name, p->name))
      break;
  assert (p->name);
  *p->valptr = p->def;
}

static void force_no_bound (void) {
  reinit_opt ("fwmaxoccs");
  reinit_opt ("fwmax1size");
  reinit_opt ("fwmax2size");
  reinit_opt ("bwmaxoccs");
  reinit_opt ("bwmax1size");
  reinit_opt ("bwmax2size");
  reinit_opt ("blkmax1occs");
  reinit_opt ("blkmax2occs");
  reinit_opt ("blkmax1size");
  reinit_opt ("blkmax2size");
  reinit_opt ("elimoccs");
  reinit_opt ("elimsize");
}

static int parse_long_opt (const char * arg) {
  const char * q, * valstr;
  int len, oldval, newval;
  Opt * p;
  if (!*arg) return 0;
  q = strchr (arg, '=');
  len = q ? q - arg : strlen (arg);
  for (p = opts; p->name; p++)
    if (strlen (p->name) == len && !strncmp (arg, p->name, len))
      break;
  if (!p->name) return 0;
  oldval = *p->valptr;
  if (q)
    {
      assert (*q == '=');
      valstr = ++q;
      if (*q == '-') q++;
      if (!isdigit (*q)) return 0;
      while (isdigit (*q)) q++;
      if (*q) return 0;
      newval = atoi (valstr);
    }
  else if (arg[len]) return 0;
  else newval = oldval + 1;
  if (newval < p->low) {
    newval = p->low;
    wrn ("capping '--%s' with minimum value %d", arg, p->low);
  }
  if (newval > p->high) {
    newval = p->high;
    wrn ("capping '--%s' with maximum value %d", arg, p->high);
  }
  *p->valptr = newval;
  if (!strcmp (p->name, "bound")) {
    assert (p->low == -1);
    if (newval == -1) force_no_bound ();
    else force_bound ();
  }
  return 1;
}

static int parse_no_long_opt (const char * arg) {
  Opt * p;
  int val;
  for (p = opts; p->name; p++)
    if (!strcmp (arg, p->name)) break;
  if (!p->name) return 0;
  if (0 < p->low) val = p->low;
  if (p->high < 0) val = p->high;
  *p->valptr = p->low;
  if (!strcmp (p->name, "bound")) force_no_bound ();
  return 1;
}

static int parse_opt (const char * arg) {
  int res;
  if (!strcmp (arg, "-n")) output = 0, res = 1;
  else if (arg[0] == '-' && arg[1] != '-') res = parse_short_opt (arg + 1);
  else if (!strncmp (arg, "--no-", 5)) res = parse_no_long_opt (arg + 5);
  else if (arg[0] == '-' && arg[1] == '-') res = parse_long_opt (arg + 2);
  else res = 0;
  return res;
}

static void embedded_opt (char * line) {
  const char * opt;
  char * p, ch;
  if (ignore) return;
  for (p = line; isspace (*p); p++)
    ;
  if (*p != '-') return;
  opt = p++;
  while (isalnum (*p) || *p == '-') p++;
  if (*p == '=') {
    p++;
    if (*p == '-') p++;
    while (isdigit (*p)) p++;
  }
  if (*p && *p != ' ' && *p != '\t' && *p != '\r') {
    ch = *p;
    *p = 0;
    wrn ("unexpected character 0x%02x after embedded option: %s", ch, opt);
    return;
  }
  *p = 0;
  if (parse_opt (opt)) {
    msg ("embedded option: %s", opt);
    embedded_options++;
  } else wrn ("invalid embedded option: %s", opt);
}

static void list_opts_values (void) {
  Opt * p;
  for (p = opts; p->name; p++)
    printf ("c [bloqqer] --%s=%d\n", p->name, *p->valptr);
}

static const char * parse (void) {
  int ch, m, n, i, c, q, lit, sign, started;
  double start;

  start = seconds ();
  msg ("reading %s", iname);

  lineno = 1;
  m = n = i = c = q = 0;
  assert (!universal_vars);
  assert (!existential_vars);
  szline = 128;
  assert (!line);
  NEWN (line, szline);
  nline = 0;
SKIP:
  ch = getc (ifile);
  if (ch == '\n') { lineno++; goto SKIP; }
  if (ch == ' ' || ch == '\t' || ch == '\r') goto SKIP;
  if (ch == 'c') {
    line[nline = 0] = 0;
    while ((ch = getc (ifile)) != '\n') {
      if (ch == EOF) return "end of file in comment";
      if (nline + 1 == szline) {
	RSZ (line, szline, 2*szline);
	szline *= 2;
      }
      line[nline++] = ch;
      line[nline] = 0;
    }
    embedded_opt (line);
    lineno++;
    goto SKIP;
  }
  if (ch != 'p') {
HERR:
    return "invalid or missing header";
  }
  if (verbose) {
    if (ignore) msg ("disabled parsing and ignored embedded options");
    else msg ("finished parsing %d embedded options", embedded_options);
    msg ("listing final option values:");
    list_opts_values ();
  }
  if (getc (ifile) != ' ') goto HERR;
  while ((ch = getc (ifile)) == ' ')
    ;
  if (ch != 'c') goto HERR;
  if (getc (ifile) != 'n') goto HERR;
  if (getc (ifile) != 'f') goto HERR;
  if (getc (ifile) != ' ') goto HERR;
  while ((ch = getc (ifile)) == ' ')
    ;
  if (!isdigit (ch)) goto HERR;
  m = ch - '0';
  while (isdigit (ch = getc (ifile)))
    m = 10 * m + (ch - '0');
  if (ch != ' ') goto HERR;
  while ((ch = getc (ifile)) == ' ')
    ;
  if (!isdigit (ch)) goto HERR;
  n = ch - '0';
  while (isdigit (ch = getc (ifile)))
    n = 10 * n + (ch - '0');
  while (ch != '\n')
    if (ch != ' ' && ch != '\t' && ch != '\r') goto HERR;
    else ch = getc (ifile);
  lineno++;
  msg ("found header 'p cnf %d %d'", m, n);
  remaining = num_vars = m;
  remaining_clauses_to_parse = n;
  NEWN (vars, num_vars + 1);
  if (num_vars) init_variables (1);
  NEWN (dfsi, 2*num_vars+1);
  dfsi += num_vars;
  NEWN (mindfsi, 2*num_vars+1);
  mindfsi += num_vars;
  NEWN (repr, 2*num_vars+1);
  repr += num_vars;
  NEWN (fwsigs, num_vars + 1);
  NEWN (bwsigs, num_vars + 1);
  NEWN (anchors, num_vars + 1);
  NEWN (trail, num_vars);
  top_of_trail = next_on_trail = trail;
  NEWN (schedule, num_vars);
  assert (!size_schedule);
  started = 0;
NEXT:
   ch = getc (ifile);
   if (ch == '\n') { lineno++; goto NEXT; }
   if (ch == ' ' || ch == '\t' || ch == '\r') goto NEXT;
   if (ch == 'c') {
     while ((ch = getc (ifile)) != '\n')
       if (ch == EOF) return "end of file in comment";
     lineno++;
     goto NEXT;
   }
   if (ch == EOF) {
     if (!force && i < n) return "clauses missing";
     orig_clauses = i;
     if (!q && !c) init_implicit_scope ();
     goto DONE;
   }
   if (ch == '-') {
     if (q) return "negative number in precix";
     sign = -1;
     ch = getc (ifile);
     if (ch == '0') return "'-' followed by '0'";
   } else sign = 1;
   if (ch == 'e') { 
     if (c) return "'e' after at least one clause";
     if (q) return "'0' missing after 'e'";
     q = 1;
     goto NEXT;
   }
   if (ch == 'a') { 
     if (c) return "'a' after at least one cluase";
     if (q) return "'0' missing after 'a'";
     q = -1; 
     goto NEXT;
   }
   if (!isdigit (ch)) return "expected digit";
   lit = ch - '0';
   while (isdigit (ch = getc (ifile)))
     lit = 10 * lit + (ch - '0');
   if (ch != EOF && ch != ' ' && ch != '\t' && ch != '\n' && ch != '\r')
     return "expected space after literal";
   if (ch == '\n') lineno++;
   if (lit > m) return "maximum variable index exceeded";
   if (!force && !q && i == n) return "too many clauses";
   if (q) {
     if (lit) {
       if (sign < 0) return "negative literal quantified";
       if (lit2scope (lit)) return "variable quantified twice";
       lit *= q;
       add_quantifier (lit);
       if (q > 0) existential_vars++; else universal_vars++;
     } else q = 0;
   }
   else {
     if (!q && !c) {
       started = 1;
       init_implicit_scope ();
       start_progress ("remaining clauses to parse",
                       &remaining_clauses_to_parse);
     }
     if (lit) lit *= sign, c++; else i++, remaining_clauses_to_parse--;
     if (lit) push_literal (lit);
     else {
       if (!empty_clause && !trivial_clause ()) {
	 add_clause ();
	 if (empty_clause) {
	   orig_clauses = i;
	   goto DONE;
	 }
       } else num_lits = 0;
      }
   }
   goto NEXT;
DONE:
  if (started) stop_progress ();
  msg ("read %d literals in %.1f seconds", c, seconds () - start);
  if (verbose) log_pruned_scopes ();
  return 0;
}

static int var2lit (Var * v) { 
  assert (vars < v && v <= vars + num_vars);
  return (int)(long)(v - vars);
}

static int map_lit (int lit) {
  Var * v = lit2var (lit);
  int res = v->mapped;
  assert (res);
  if (lit < 0) res = -res;
  return res;
}

static int trail_flushed (void) {
  return next_on_trail == top_of_trail;
}

static void flush_pos (int lit) {
  Occ * occ = lit2occ (lit);
  Node * p, * next;
  LOG ("flushing positive occurrences of %d", lit);
  for (p = occ->first; p; p = next) {
    next = p->next;
    delete_clause (p->clause);
  }
  assert (!occ->count);
}

static void flush_node (Node * node) {
  int lit = node->lit, other, found;
  Clause * clause = node->clause;
  Node * p;
  assert (!num_lits);
  for (p = clause->nodes; (other = p->lit); p++) {
    if (other == lit) { found = 1; continue; }
    assert (num_lits < size_lits);
    lits[num_lits++] = other;
  }
  assert (found);
  assert (num_lits + 1 == clause->size);
  if (trivial_clause ()) num_lits = 0;
  else add_clause ();
  delete_clause (clause);
}

static void flush_neg (int lit) {
  Occ * occ = lit2occ (-lit);
  Node * p, * next;
  LOG ("flushing negative occurrences of %d", lit);
  for (p = occ->first; p; p = next) {
    next = p->next;
    flush_node (p);
  }
  assert (!occ->count);
}

static void flush_lit (int lit) {
  flush_pos (lit);
  flush_neg (lit);
  assert (null_occurrences (lit));
}

static void flush_trail (void) {
  Clause * p, * next;
  int idx, lit;
  Var * v;
  while (!empty_clause && !trail_flushed ()) {
    lit = *next_on_trail++;
    flush_lit (lit);
  }
  if (empty_clause) {
    for (p = first_clause; p; p = next) {
      next = p->next;
      if (p == empty_clause) continue;
      delete_clause (p);
      subsumed_clauses++;
    }
    for (idx = 1; idx <= num_vars; idx++) {
      v = vars + idx;
      assert (null_occurrences (idx));
      if (v->tag != FREE) continue;
      zombie (idx);
    }
    next_on_trail = top_of_trail;
  }
  assert (trail_flushed ());
}

static int pop_schedule (void) {
  int res, lpos, last;
  Var * rvar, * lvar;
  assert (size_schedule > 0);
  res = schedule[0];
  rvar = lit2var (res);
  assert (!rvar->pos);
  rvar->pos = -1;
  lpos = --size_schedule;
  if (lpos > 0) {
    last = schedule[lpos];
    lvar = lit2var (last);
    assert (lvar->pos == lpos);
    schedule[0] = last;
    lvar->pos = 0;
    down (last);
  }
  LOG ("pop %d with score %d", res, rvar->score);
  return res;
}

static void mark_clause (Clause * clause) {
  Node * p;
  int lit;
  check_all_unmarked ();
  for (p = clause->nodes; (lit = p->lit); p++)
    mark_lit (lit);
}

static void unmark_clause (Clause * clause) {
  Node * p;
  int lit;
  for (p = clause->nodes; (lit = p->lit); p++)
    unmark_lit (lit);
  check_all_unmarked ();
}

static int block_clause_aux (int pivot) {
  int res, found, lit, tmp, porder, lorder;
  Clause * other;
  Node * p, * q;
  Occ * occ;
  Var * v;
  assert (existential (pivot));
  res = 1;
  occ = lit2occ (-pivot);
  porder = lit2scope (pivot)->order;
  for (p = occ->first; res && p; p = p->next) {
    assert (p->lit == -pivot);
    other = p->clause;
    assert (other->size >= 2);
    if (other->size > blkmax2size) { res = 0; continue; }
    found = 0;
    LOGCLAUSE (other, "try to resolve on %d with other clause", pivot);
    for (q = other->nodes; (lit = q->lit); q++) {
      if (lit == -pivot) { assert (p == q); found = 1; continue; }
      lorder = lit2scope (lit)->order;
      if (lorder > porder) continue;//TODO triangle dependencies?
      v = lit2var (lit);
      tmp = v->mark;
      if (tmp == -lit) break;
    }
    assert (lit || found);
    if (lit) LOG ("other clause produces trivial resolvent on %d", lit);
    else { LOG ("other clause produces non-trivial resolvent"); res = 0; }
  }
  return res;
}

static int block_clause (Clause * clause, int pivot) {
  Occ * occ;
  int res;
  assert (existential (pivot));
  if (clause->size > blkmax1size) return 0;
  occ = lit2occ (-pivot);
  if (occ->count > blkmax1occs) return 0;
  LOGCLAUSE (clause, "check whether literal %d blocks clause", pivot);
  mark_clause (clause);
  res = block_clause_aux (pivot);
  unmark_clause (clause);
  return res;
}

static void block_lit (int lit) {
  Node * p, * next;
  Clause * clause;
  Occ * occ;
  assert (existential (lit));
  if (!bce) return;
  occ = lit2occ (lit);
  if (occ->count > blkmax2occs) return;
  LOG ("CHECKING %d as blocking literal", lit);
  for (p = occ->first; trail_flushed () && p; p = next) {
    next = p->next;
    clause = p->clause;
    if (!block_clause (clause, lit)) continue;
    LOGCLAUSE (clause, "literal %d blocks clause", lit);
    delete_clause (clause);
    blocked_clauses++;
  }
}

static int try_to_resolve_away (int limit) {
  int elimidx_order, lit_order, lit, mini_scope, count, nontriv, found, res;
  int clash = 0, nonstrictve = 0;
  Node * p, * q, * r;
  Occ * pocc, * nocc;
  Clause * c, * d;
  int cpos, dpos;
  assert (trail_flushed ());
  assert (existential (elimidx));
  assert (!deref (elimidx));
  assert (limit < INT_MAX-1);
  if (!ve) return 0;
  elimidx_order = lit2scope (elimidx)->order;
  nontriv = count = 0;
  pocc = lit2occ (elimidx);
  nocc = lit2occ (-elimidx);
  for (p = pocc->first;
       trail_flushed () && nontriv <= limit && p;
       p = p->next) {
    c = p->clause;
    if (c->size > elimsize) { nontriv = INT_MAX-1; continue; }
    mark_clause (c);
    mini_scope = 1;
    for (q = nocc->first;
         trail_flushed () && nontriv <= limit && q;
	 q = q->next) {
      count++;
      d = q->clause;
      if (d->size > elimsize) { nontriv = INT_MAX-1; continue; }
      LOGCLAUSE (c, "%d:%d/%d elimination %d clause",
                count, nontriv + 1, limit, elimidx);
      LOGCLAUSE (d, "%d:%d/%d elimination %d clause",
		 count, nontriv + 1, limit, -elimidx);
      clash = found = 0;
      for (r = d->nodes; (lit = r->lit); r++) {
	if (lit == -elimidx) { found = 1; continue; }
	lit_order = lit2scope (lit)->order;
	if (lit_order > elimidx_order) { 
	  if (mini_scope) 
	    LOG ("literal %d in scope %d prevents mini-scoping", 
	         lit, lit_order);
	  mini_scope = 0; 
	} else if (clash) continue;
	else if (lit2var (lit)->mark == -lit) clash = lit;
      }
      assert (found);
      // COVER (clash && !mini_scope);
      if (clash) {
	LOG ("literal %d in scope %d makes resolvent trivial",
	     clash, lit2order (clash));
	if (!mini_scope) {
	  if (strict) {
	    LOG ("but strict variable elimination prevents resolution");
	    nontriv = INT_MAX;
	  } else {
	    LOG ("non-strict mini-scoping applied");
	    nonstrictve = 1;
	  }
	}
      } else if (mini_scope) {
	LOG ("non-trivial resolvent can be mini-scoped");
	nontriv++;
	if (c->size == 2 && d->size == 2) {
	  cpos = abs (c->nodes[1].lit) == elimidx;
	  dpos = abs (d->nodes[1].lit) == elimidx;
	  lit = c->nodes[!cpos].lit;
	  if (d->nodes[!dpos].lit == lit) {
	    LOG ("unit %d during temptative resolution", lit);
	    units++;
	    assign (lit);
	  }
	}
      } else {
        assert (!mini_scope);
	assert (!clash || strict);
	LOG ("non-trivial resolvent can NOT be mini-scoped");
	nontriv = INT_MAX;
      }
    }
    unmark_clause (c);
  }
  if (trail_flushed ()) {
    res = (nontriv <= limit);
#ifndef NLOG
    if (nontriv <= limit) 
      LOG ("resolving away %d will generate only %d clauses not exceeding %d",
	   elimidx, nontriv, limit);
    else if (nontriv < INT_MAX-1)
      LOG ("resolving away %d would generate at least one additional clause",
	   elimidx);
    else if (nontriv == INT_MAX-1) {
      LOG ("resolving away %d involves too large clauses", elimidx);
    } else {
      assert (nontriv == INT_MAX);
      LOG ("mini-scoping and resolving away %d is impossible", elimidx);
    }
#endif
  } else {
    LOG ("unit resolvent stops temptatively resolving away %d", elimidx);
    res = 0;
  }
  if (res && nonstrictve) nonstrictves++;
  return res;
}

static void resolve_away (int idx) {
  Node * p, * q, * r, * next;
  Clause * c, * d;
  int lit;
  assert (elimidx > 0);
  assert (!deref (elimidx));
  assert (trail_flushed ());
  LOG ("RESOLVING away %d", elimidx);
  for (p = lit2occ (elimidx)->first; p; p = p->next) {
    c = p->clause;
    for (q = lit2occ (-elimidx)->first; q; q = q->next) {
      d = q->clause;
      assert (!num_lits);
      LOGCLAUSE (c, "%d antecedent", elimidx);
      for (r = c->nodes; (lit = r->lit); r++)
	if (abs (lit) != elimidx)
	  push_literal (lit);
      LOGCLAUSE (d, "%d antecedent", -elimidx);
      for (r = d->nodes; (lit = r->lit); r++)
	if (abs (lit) != elimidx)
	  push_literal (lit);
      if (trivial_clause ()) { LOG ("resolvent is trivial"); num_lits = 0; }
      else { LOG ("non-trivial"); add_clause (); }
    }
  }
  LOG ("deleting clauses with %d", elimidx);
  for (p = lit2occ (elimidx)->first; p; p = next) {
    next = p->next;
    delete_clause (p->clause);
  }
  LOG ("deleting clauses with %d", -elimidx);
  for (p = lit2occ (-elimidx)->first; p; p = next) {
    next = p->next;
    delete_clause (p->clause);
  }
  eliminated++;
  assert (isfree (elimidx));
  tag_lit (elimidx, ELIMINATED);
  assert (remaining > 0);
  remaining--;
  LOG ("eliminated %d remaining %d", elimidx, remaining);
}

static void elim_idx (int idx) {
  int limit;
  assert (0 < idx);
  assert (!deref (idx));
  assert (!elimidx);
  if(bce) LOG ("TRYING to block clauses on idx %d", idx);
  block_lit (idx);
  if (trail_flushed ()) {
    block_lit (-idx);
    if (trail_flushed ()) {
      limit = lit2occ (idx)->count + lit2occ (-idx)->count;
      if (limit <= elimoccs) {
	limit += excess;
	LOG ("TRYING to eliminate %d clauses with idx %d in scope %d", 
	     limit, idx, lit2scope (idx)->order);
	assert (!elimidx);
	elimidx = idx;
	if (try_to_resolve_away (limit)) resolve_away (idx);
	assert (elimidx == idx);
	elimidx = 0;
      }
    }
  }
}

static int backward_subsumes (Clause * clause, Clause * other) {
  int lit, count, except, tmp;
  Node * p;
  assert (clause != other);
  assert (clause->size <= bwmax1size);
  if (other->size > bwmax2size) return 0;
  if (clause->size >= other->size) return 0;
  bw.sig1.lookups++;
  if (clause->sig & ~other->sig) { bw.sig1.hits++; return 0; }
  count = clause->size;
  except = other->size - count;
  for (p = other->nodes; except >= 0 && (lit = p->lit); p++)
    if ((tmp = lit2var (lit)->submark) == lit) {
      if (!--count) return 1;
    } else if (tmp == -lit) return 0;
    else except--;
  return 0;
}

static int backward_self_subsumes (Clause * clause, Clause * other) {
  int lit, count, tmp, except, res;
  Node * p;
  assert (clause != other);
  assert (clause->size <= bwmax1size);
  if (other->size > bwmax2size) return 0;
  if (clause->size > other->size) return 0;
  bw.sig1.lookups++;
  if (clause->sig & ~other->sig) { bw.sig1.hits++; return 0; }
  count = clause->size;
  except = other->size - count;
  res = 0;
  for (p = other->nodes; except >= 0 && (lit = p->lit); p++)
    if ((tmp = lit2var (lit)->submark) == lit) {
      if (!--count) return res;
    } else if (tmp == -lit) {
      if (res) return 0;
      res = lit;
      if (!--count) return res;
    } else except--;
  return 0;
}

static void check_all_unsubmarked (void) {
#if 0
#warning "expensive checking that all 'submark' flags are clear enabled"
  int idx;
  for (idx = 1; idx <= num_vars; idx++)
    assert (!vars[idx].submark);
#endif
}

static void submark_clause (Clause * clause) {
  Node * p;
  int lit;
  check_all_unsubmarked ();
  for (p = clause->nodes; (lit = p->lit); p++)
    submark_lit (lit);
}

static void unsubmark_clause (Clause * clause) {
  Node * p;
  int lit;
  for (p = clause->nodes; (lit = p->lit); p++)
    unsubmark_lit (lit);
  check_all_unsubmarked ();
}

static void backward_strengthen (Clause * clause, int lit) {
  int found, other;
  Node * p;
  LOGCLAUSE (clause,
             "backward strengthening by removing %d from clause", lit);
  found = 0;
  assert (!num_lits);
  for (p = clause->nodes; (other = p->lit); p++) {
    if (other == lit) { found = 1; continue; }
    assert (num_lits < num_vars);
    lits[num_lits++] = other;
  }
  assert (found);
  assert (num_lits + 1 == clause->size);
  if (trivial_clause ()) num_lits = 0;
  else add_clause ();
  delete_clause (clause);
  strengthened_clauses++;
  backward_strengthened_clauses++;
}

static int least_occurring_lit_except (Clause * clause, int except) {
  int best = INT_MAX, tmp, start = 0, lit;
  Node * p;
  assert (clause->size > 1);
  for (p = clause->nodes; (lit = p->lit); p++) {
    if (lit == except) continue;
    tmp = lit2occ (lit)->count;
    if (best <= tmp) continue;
    start = lit;
    best = tmp;
  }
  assert (start);
  assert (best < INT_MAX);
  LOG ("literal %d has shortest occurrence list of length %d except %d",
       start, best, except);
  assert (best >= 1);
  return start;
}

static void backward (Clause * clause) {
  int lit, first, second, i;
  Node * p, * next;
  Clause * other;
  Occ * occ;
  Sig sig;
  if (clause->size > bwmax1size) return;
  first = least_occurring_lit_except (clause, 0);
  if (lit2occ (first)->count > bwmaxoccs) return;
  sig = ~0llu;
  for (p = clause->nodes; (lit = p->lit); p++)
    sig &= bwsigs [ abs (lit) ];
  LOG ("clause sig2  %016llx", clause->sig);
  LOG ("intersection %016llx", sig);
  bw.sig2.lookups++;
  if (clause->sig & ~sig) { bw.sig2.hits++; goto DONE; }
  LOGCLAUSE (clause, "backward subsumption with clause");
  submark_clause (clause);
  occ = lit2occ (first);
  for (p = occ->first; p; p = next) {
    next = p->next;
    other = p->clause;
    if (other == clause) continue;
    if (!backward_subsumes (clause, other)) continue;
    subsumed_clauses++;
    backward_subsumed_clauses++;
    LOGCLAUSE (other, "backward subsumed clause");
    delete_clause (other);
  }
  for (i = 1; i <= 2; i++) {
    if (i == 2) {
      second = least_occurring_lit_except (clause, first);
      occ = lit2occ (second);
    }
    for (p = occ->first; p; p = next) {
      next = p->next;
      other = p->clause;
      if (other == clause) continue;
      lit = backward_self_subsumes (clause, other);
      if (!lit) continue;
      LOG ("backward self subsuming resolution on %d", lit);
      LOGCLAUSE (clause, "1st backward self subsuming antecendent");
      LOGCLAUSE (other, "2nd backward self subsuming antecendent");
      assert (clause->size <= other->size);
      if (clause->size == other->size) {
	backward_strengthen (other, lit);
	unsubmark_clause (clause);
LOG ("double backward self subsumption strengthens subsuming clause");
	backward_strengthen (clause, -lit);
	clause = 0;
	return;
      } else backward_strengthen (other, lit);
    }
  }
  unsubmark_clause (clause);
DONE:
  assert (clause);
  for (p = clause->nodes; (lit = p->lit); p++)
    bwsigs [ abs (lit) ] |= clause->sig;
}

static void enlarge_stack (void) {
  int new_size = szstack ? 2*szstack : 1;
  RSZ (stack, szstack, new_size);
  szstack = new_size;
}

static void push_stack (int elem) {
  if (szstack == nstack) enlarge_stack ();
  stack[nstack++] = elem;
}

static void enlarge_aux (void) {
  int new_size = szaux ? 2*szaux : 1;
  RSZ (aux, szaux, new_size);
  szaux = new_size;
}

static void push_aux (int elem) {
  if (szaux == naux) enlarge_aux ();
  aux[naux++] = elem;
}

static int hidden_tautology (Clause * c) {
  int redundant, lit, other, next, props, add, found, tmp, i, j, order;
  Node * p, * q, * r;
  Occ * pocc, * nocc;
  Clause * d;
  if (!hte && !cce) return 0;
  LOGCLAUSE (c, "trying hidden tautology elimination of clause");
  assert (!nstack);
  for (p = c->nodes; (lit = p->lit); p++) 
    push_stack (lit), mark_lit (lit);
  add = redundant = other = next = props = 0;
  while (next < nstack) {
    if (++props > htesteps) goto DONE;
    lit = stack[next++];
    assert (lit2var (lit)->mark == lit);
    pocc = lit2occ (lit);
    if (hte && pocc->count <= hteoccs)
      for (q = pocc->first; q; q = q->next) {
	d = q->clause;
	if (d == c) continue;
	if (d->size > htesize) continue;
	found = add = 0;
	for (r = d->nodes; (other = r->lit); r++) {
	  if (other == lit) { found = 1; continue; }
	  assert (other != lit);
	  tmp = lit2var (other)->mark;
	  if (tmp == other) continue;
	  if (add || tmp == -other) { add = INT_MAX; break; }
	  add = -other;
	}
	assert (other || add == INT_MAX || found);
	if (add == INT_MAX) continue;
	if (!add) { 
	  redundant = 1;
	  LOGCLAUSE (d, "hiddenly subsuming clause");
	  goto DONE; 
	}
	LOGCLAUSE (d, "added hidden literal %d through clause", add);
	hlas++;
	tmp = lit2var (add)->mark;
	if (tmp == -add) { redundant = 1; goto DONE; }
	assert (!tmp);
	mark_lit (add);
	push_stack (add);
      }
    if (cce && existential (lit)) {
      nocc = lit2occ (-lit);
      q = nocc->first;
      if (q && nocc->count <= hteoccs) {
	assert (!naux);
	order = lit2order (lit);
	found = 0;
	d = q->clause;
	for (r = d->nodes; (other = r->lit); r++) {
	  if (other == -lit) { found = 1; continue; }
	  if (lit2var (other)->mark == other) continue;
	  if (lit2order (other) > order) continue;
	  submark_lit (other);
	  push_aux (other);
	}
	assert (found);
	for (q = q->next; naux && q; q = q->next) {
	  d = q->clause;
	  found = 0;
	  for (r = d->nodes; (other = r->lit); r++) {
	    if (other == -lit) { found = 1; continue; }
	    if (lit2var (other)->mark == other) continue;
	    if (lit2order (other) > order) continue;
	    tmp = lit2var (other)->submark;
	    if (tmp != other) continue;
	    unsubmark_lit (other);
	  }
	  j = 0;
	  for (i = 0; i < naux; i++) {
	    other = aux[i];
	    tmp = lit2var (other)->submark;
	    if (tmp == other) {
	      unsubmark_lit (other);
	    } else {
	      assert (!tmp);
	      submark_lit (other);
	      aux[j++] = other;
	    }
	  }
	  naux = j;
	}
	while (naux > 0) {
	  other = aux[--naux];
	  unsubmark_lit (other);
	  if (redundant) continue;
	  tmp = lit2var (other)->mark;
	  LOG ("adding covered literal %d for pivot %d", other, lit);
	  clas++;
	  if (tmp) { assert (tmp == -other); redundant = 1; continue; }
	  mark_lit (other);
	  push_stack (other);
	}
	check_all_unsubmarked ();
      }
    }
  }
DONE:
  LOG ("hidden tautology check finished after %d steps", props);
  if (redundant) {
    LOGCLAUSE (c, "hidden tautological clause");
    hidden_tautologies++;
  } else if (hbce && bce) {
    for (i = 0; !redundant && i < nstack; i++) {
      lit = stack[i];
      if (!existential (lit)) continue;
      if (lit2occ (-lit)->count > blkmax1occs) continue;
      LOG ("check whether literal %d blocks extended clause", lit);
      redundant = block_clause_aux (lit);
    }
    if (redundant) {
      hidden_blocked_clauses++;
      LOGCLAUSE (c, "hidden blocked clause");
    }
  }
  while (nstack > 0) unmark_lit (stack[--nstack]);
  check_all_unmarked ();
  check_all_unsubmarked ();
  if (!redundant) return 0;
  delete_clause (c);
  return 1;
}

#if 0 && !defined(NDEBUG)
#warning "expensive subsumed checking code enabled"
static int really_subsumes (Clause * a, Clause * b) {
  int res, lit;
  Node * p;
  if (a->size > b->size) return 0;
  mark_clause (b);
  res = 1;
  for (p = a->nodes; res && (lit = p->lit); p++)
    res = (lit2var (lit)->mark == lit);
  unmark_clause (b);
  return res;
}

static int really_strengthen (Clause * a, Clause * b) {
  int res, lit, tmp, pivot;
  Node * p;
  if (a->size > b->size) return 0;
  mark_clause (b);
  res = 1;
  pivot = 0;
  for (p = a->nodes; res && (lit = p->lit); p++) {
    tmp = lit2var (lit)->mark;
    if (tmp == lit) continue;
    else if (tmp != -lit) res = 0;
    else if (pivot) res = 0;
    else pivot = lit;
  }
  unmark_clause (b);
  return res ? pivot : 0;
}

static void check_subsumed (void) {
  Clause * p, * q;
  for (p = first_clause; p; p = p->next)
    for (q = first_clause; q; q = q->next) {
      if (p != q) {
	assert (!really_subsumes (p, q));
	assert (!really_strengthen (p, q));
      } else assert (really_subsumes (p, q));
    }
}
#else
static void check_subsumed (void) { }
#endif

static void flush_queue (int outer) {
  Clause * clause;
  double start;
  if (!queue) return;
  start = outer ? seconds () : 0.0;
  LOG ("starting to flush queue with %d clauses", nqueue);
  if (outer) start_progress ("backward subsumption queue", &nqueue);
  while ((clause = queue)) {
    dequeue (clause);
    if (!hidden_tautology (clause)) backward (clause);
    flush_trail ();
    if (empty_clause) break;
  }
  assert (empty_clause || !nqueue);
  if (outer) stop_progress ();
  check_subsumed ();
  if (!outer) return;
  msg ("flushed backward subsumption queue in %.1f seconds",
       seconds () - start);
  if (verbose) log_pruned_scopes ();
}

static int pop_literal (void) {
  assert (num_lits > 0);
  return lits[--num_lits];
}

static void dfs_clean (void) {
  memset (dfsi - num_vars, 0, (2*num_vars + 1) * sizeof *dfsi);
  memset (mindfsi - num_vars, 0, (2*num_vars + 1) * sizeof *mindfsi);
  memset (repr - num_vars, 0, (2*num_vars + 1) * sizeof *repr);
}

static int cmporder (int a, int b) {
  int res;
  if ((res = lit2order (a) - lit2order (b))) return res;
  if ((res = abs (a) - abs (b))) return res;
  return b - a;
}

static int subst_clause (Clause * c) {
  int lit, tmp, res;
  Node * p;
  assert (!num_lits);
  res = 0;
  for (p = c->nodes; (lit = p->lit); p++) {
    tmp = repr[lit];
    assert (tmp);
    if (tmp != lit) res = 1;
    push_literal (tmp);
  }
  if (!res) { num_lits = 0; return 0; }
  LOGCLAUSE (c, "substituting clause");
  if (trivial_clause ()) {
    LOG ("substituted clause becomes trivial");
    num_lits = 0;
  } else add_clause ();
  return 1;
}

static void flush_vars (void) {
  int idx, pos, neg;
  for (idx = 1; !empty_clause && idx <= num_vars; idx++) {
    if (!isfree (idx)) continue;
    pos = lit2occ (idx)->count; 
    neg = lit2occ (-idx)->count; 
    if (!pos && !neg) zombie (idx);
    if (existential (idx)) {
      if (!neg) unet (idx); else if (!pos) unet (-idx);
    } else {
      if (!neg) unate (idx); else if (!pos) unate (-idx);
    }
  }
}

static void subst (void) {
  Clause * last, * p, * next, * prev;
  int idx, tmp, count = 0;
  Var * v;
  assert (!empty_clause);
  assert (!num_lits);
  assert (!substituting);
  for (idx = 1; idx <= num_vars; idx++) {
    tmp = repr[idx];
    if (!tmp) continue;
    if (tmp == idx) continue;
    v = vars + idx;
    assert (v->tag == FREE);
    substituted++;
    tag_var (v, SUBSTITUTED);
    assert (remaining > 0);
    remaining--;
    LOG ("substituting %d by %d remaining %d", idx, tmp, remaining);
  }
  substituting = 1;
  last = last_clause;
  prev = 0;
  for (p = first_clause; !empty_clause && prev != last; p = next) {
    next = p->next;
    prev = p;
    if (!subst_clause (p)) continue;
    delete_clause (p);
    count++;
  }
  LOG ("substituted %d clauses", count);
  assert (substituting);
  substituting = 0;
  flush_vars ();
}

static int eqres (int outer) {
  int start, current, idx, count, pos, child, min, max, tmp, i;
  double started;
  Clause * c;
  Occ * occ;
  Node * p;
  if (!eq) return 0;
  assert (added_binary_clauses_at_last_eqround <= added_binary_clauses);
  if (added_binary_clauses_at_last_eqround == added_binary_clauses) return 0;
  added_binary_clauses_at_last_eqround = added_binary_clauses;
  assert (!empty_clause);
  assert (trail_flushed ());
  assert (!queue);
  started = seconds ();
  eqrounds++;
  idx = count = 0;
  for (start = -num_vars; !empty_clause && start <= num_vars; start++) {
    if (!start) continue;
    if (!isfree (start)) continue;
    if (dfsi[start]) continue;
    assert (!num_lits);
    assert (!nstack);
    push_literal (start);
    while (num_lits > 0) {
      current = pop_literal ();
      if (current) {
	if (dfsi[current]) continue;
	push_stack (current);
	push_literal (current);
	push_literal (0);
	mindfsi[current] = dfsi[current] = ++idx;
	assert (idx < INT_MAX);
	LOG ("dfsi %d = %d", current, idx);
	occ = lit2occ (-current);
	for (p = occ->first; p; p = p->next) {
	  c = p->clause;
	  if (c->size != 2) continue;
	  assert (p->lit == -current);
	  pos = (c->nodes[1].lit == -current);
	  assert (c->nodes[pos].lit == -current);
	  child = c->nodes[!pos].lit;
	  if (dfsi[child]) continue;
	  push_literal (child);
	}
      } else {
	current = pop_literal ();
	min = mindfsi[current];
	assert (min == dfsi[current]);
	occ = lit2occ (-current);
	for (p = occ->first; p; p = p->next) {
	  c = p->clause;
	  if (c->size != 2) continue;
	  assert (p->lit == -current);
	  pos = (c->nodes[1].lit == -current);
	  assert (c->nodes[pos].lit == -current);
	  child = c->nodes[!pos].lit;
	  tmp = mindfsi[child];
	  if (tmp >= min) continue;
	  min = tmp;
	}
	mindfsi[current] = min;
#ifndef NLOG
	{
	  int cmpch = '=';
	  if (min < dfsi[current]) cmpch = '<';
	  if (min > dfsi[current]) cmpch = '>';
	  LOG ("mindfsi %d = %d   %c%c   %d = dfsi %d", 
	       current, min, cmpch, cmpch, dfsi[current], current);
	}
#endif
	assert (min <= dfsi[current]);
	if (min < dfsi[current]) continue;
	pos = nstack;
	while (stack[--pos] != current)
	  assert (pos > 0);
	max = 0;
	for (i = pos; i < nstack; i++)
	  if (universal (tmp = stack[i]) && 
	      (!max || cmporder (max, tmp) < 0)) max = tmp;
	min = current;
	for (i = pos + 1; i < nstack; i++)
	  if (cmporder ((tmp = stack[i]), min) < 0) min = tmp;
	for (i = pos; !empty_clause && i < nstack; i++) {
	  repr[tmp = stack[i]] = min;
	  if (tmp != min) count++;
	  mindfsi[tmp] = INT_MAX;
	  LOG ("repr %d = %d", tmp, min);
	  if (max &&
	      tmp != max && 
	      lit2order (tmp) <= lit2order (max)) {
            LOG ("universal representative %d of scope %d", 
	         max, lit2order (max));
	    LOG ("literal %d of scope %d is equivalent to %d",
	         tmp, lit2order (tmp), max);
GENERATE_EMPTY_CLAUSE:
            num_lits = 0;
	    LOG ("forced to add empty clause");
	    add_clause ();
	  } else if (tmp == -min) {
	    LOG ("%d and %d are both in the same equivalence class",
	         tmp, min);
	    goto GENERATE_EMPTY_CLAUSE;
	  }
	}
	nstack = pos;
      }
    }
  }
  LOG ("found %d equivalent literals", count);
  assert (empty_clause || !(count & 1));
  assert (empty_clause || (!num_lits && !nstack));
  num_lits = nstack = 0;
  if (!empty_clause && count) subst ();
  dfs_clean ();
  if (outer || verbose > 1) {
    msg ("found %d equivalent variables in round %d in %.1f seconds",
         count/2, eqrounds, seconds () - started);
    if (verbose) log_pruned_scopes ();
  }
  if (empty_clause) flush_trail ();
  return count;
}

static void check_var_stats (void) {
#ifndef NDEBUG
  int sum_stats = remaining;
  sum_stats += unets;
  sum_stats += unates;
  sum_stats += fixed;
  sum_stats += zombies;
  sum_stats += eliminated;
  sum_stats += substituted;
  sum_stats += expanded;
  assert (sum_stats == num_vars);
#endif
}

static void flush (int outer) {
  flush_trail ();
  if (empty_clause) return;
  flush_queue (outer);
}

static void elim (void) {
  double start = seconds ();
  int idx;
  assert (!empty_clause);
  assert (trail_flushed ());
  assert (!queue);
  start_progress ("elimination queue", &size_schedule);
  do {
    while (!empty_clause && size_schedule) {
      check_var_stats ();
      idx = pop_schedule ();
      if (!isfree (idx)) continue;
      if (null_occurrences (idx)) { zombie (idx); continue; }
      if (universal (idx)) continue;
      elim_idx (idx);
      flush (0);
    }
    if (!empty_clause && eqres (0)) flush (0);
  } while (!empty_clause && size_schedule);
  stop_progress ();
  msg ("elimination took %.1f seconds", seconds () - start);
}

static int lit2stretch (int lit) { return lit2scope (lit)->stretch; }

static int expand_cost_trav (int pivot, int bound) {
  int next, lit, other, tmp, res, sign, occs, stretch;
  Node * p, * q;
  Clause * c;
  Occ * occ;
  Var * v;
  assert (universal (pivot));
  assert (0 < pivot && pivot <= num_vars);
  assert (!nstack);
  occs = lit2occ (pivot)->count + lit2occ (-pivot)->count;
  LOG ("universal %d occurs in %d clauses", pivot, occs);
  res = -occs;
  push_stack (pivot);
  mark_lit (pivot);
  next = 0;
  expansion_cost_mark++;
  assert (expansion_cost_mark > 0);
  stretch = lit2stretch (pivot);
  LOG ("checking expansion of %d in scope %d stretching to %d", 
       pivot, lit2order (pivot), stretch);
  while (next < nstack) {
    lit = stack[next++];
    assert (0 < lit && lit <= num_vars);
    v = vars + lit;
    for (sign = 0; sign <= 1; sign++) {
      occ = v->occs + sign;
      for (p = occ->first; p; p = p->next) {
	c = p->clause;
	assert (c->mark <= expansion_cost_mark);
	if (c->mark == expansion_cost_mark) continue;
	c->mark = expansion_cost_mark;
	res++;
	assert (res < INT_MAX);
	if (res > bound) {
	  LOG ("expansion of %d needs at least %d > %d clauses",
	       pivot, res, bound);
	  return INT_MAX;
	}
	for (q = c->nodes; (other = abs (q->lit)); q++) {
	  tmp = lit2var (other)->mark;
	  if (tmp) { assert (tmp == other); continue; }
	  tmp = lit2stretch (other);
	  if (tmp <= stretch) continue;
	  LOG ("expansion candidate %d depends on "
	       "variable %d in scope %d stretching to %d", 
	       pivot, other, lit2order (other), tmp);
	  if (universal (other)) {
	    assert (tmp > stretch);
	    LOG ("universal variable %d in scope %d stretching to %d "
	         "prohibits expansion of %d",
	         other, lit2order (other), tmp, pivot);
	    return INT_MAX;
	  }
	  push_stack (other);
	  mark_lit (other);
	}
      }
    }
  }
  assert (nstack > 0);
  LOG ("scope of universal %d has %d existential variables and %d clauses",
       pivot, nstack-1, res + occs);
  LOG ("expansion cost of %d would be at most %d clauses", pivot, res);
  return res;
}

static void expand_cost_clear (void) {
  while (nstack > 0) unmark_lit (stack[--nstack]);
  check_all_unmarked ();
}

static int expand_cost (int pivot, int bound) {
  int res = expand_cost_trav (pivot, bound);
  expand_cost_clear ();
  return res;
}

static void stretch_scopes (void) {
  Scope * p, * q, * next;
  for (p = outer_most_scope; p; p = p->inner) {
    q = p;
    while ((next = q->inner) && 
           (!next->free || next->type == p->type))
      q = next;
    while (p != q) {
      p->stretch = q->order;
      if (p->free)
	LOG ("scope %d actually stretches until scope %d",
	     p->order, q->order);
      else
	LOG ("empty scope %d also stretches until scope %d",
	     p->order, q->order);
      p = p->inner;
    }
  }
}

static void * fix_ptr (void * ptr, long delta) { 
  if (!ptr) return 0;
  return delta + (char*) ptr; 
}

static void fix_vars (long delta) {
  Var * v;
  Scope * s;
  for (s = outer_most_scope; s; s = s->inner) {
    v = s->first;
    if (!v) continue;
    s->first = fix_ptr (v, delta);
    s->last = fix_ptr (s->last, delta);
    for (v = s->first; v; v = v->next) {
      v->prev = fix_ptr (v->prev, delta);
      v->next = fix_ptr (v->next, delta);
    }
  }
}

static void enlarge_vars (int new_num_vars) {
  char * old_vars, * new_vars;
  int count, first_new_var;
  long delta;
  LOG ("enlarging variables from %d to %d", num_vars, new_num_vars);
  assert (num_vars <= new_num_vars);
  old_vars = (char*) vars;
  RSZ (vars, num_vars + 1, new_num_vars + 1);
  new_vars = (char*) vars;
  delta = new_vars - old_vars;
  if (delta) fix_vars (delta);
  dfsi -= num_vars;
  RSZ (dfsi, 2*num_vars + 1, 2*new_num_vars + 1);
  dfsi += new_num_vars;
  mindfsi -= num_vars;
  RSZ (mindfsi, 2*num_vars + 1, 2*new_num_vars + 1);
  mindfsi += new_num_vars;
  repr -= num_vars;
  RSZ (repr, 2*num_vars + 1, 2*new_num_vars + 1);
  repr += new_num_vars;
  RSZ (bwsigs, num_vars + 1, new_num_vars + 1);
  RSZ (fwsigs, num_vars + 1, new_num_vars + 1);
  RSZ (anchors, num_vars + 1, new_num_vars + 1);
  assert (next_on_trail == top_of_trail);
  count = top_of_trail - trail;
  RSZ (trail, num_vars + 1, new_num_vars + 1);
  top_of_trail = next_on_trail = trail + count;
  RSZ (schedule, num_vars + 1, new_num_vars + 1);
  first_new_var = num_vars + 1;
  remaining += new_num_vars - num_vars;
  num_vars = new_num_vars;
  init_variables (first_new_var);
}

static int expand_lit (int lit) {
  int res = lit2var (lit)->expcopy;
  if (!res) return lit;
  if (lit < 0) res = -res;
  return res;
}

static void expand_clause (Clause * c, int pivot) {
  int lit, other;
  Node * p;
  assert (c->mark == expansion_cost_mark);
  assert (!num_lits);
  for (p = c->nodes; (lit = p->lit); p++) {
    if (lit == pivot) continue;
    if (lit == -pivot) { num_lits = 0; return; }
    other = expand_lit (lit);
    push_literal (other);
  }
  LOGCLAUSE (c, "expanding clause");
  if (trivial_clause ()) num_lits =0 ;
  else add_clause ();
}

static void expand (int pivot, int expected) {
  int max_cost, ncopied, i, idx, first_new_var;
  Clause * c, * last;
  Var * v;
  assert (isfree (pivot));
  LOG ("expanding %d with expected cost %d", pivot, expected);
  max_cost = expand_cost_trav (pivot, expected + 1);
  assert (nstack && stack[0] == pivot);
  assert (max_cost == expected);
  for (ncopied = 0; ncopied + 1 < nstack; ncopied++) {
    idx = stack[ncopied + 1];
    v = vars + idx;
    assert (!v->expcopy);
    v->expcopy = num_vars + ncopied + 1;
    LOG ("will copy %d to %d in expansion", idx, v->expcopy);
  }
  expand_cost_clear ();
  first_new_var = num_vars + 1;
  enlarge_vars (num_vars + ncopied);
  assert (inner_most_scope->type > 0);
  for (i = 0; i < ncopied; i++) {
    idx = stack[i + 1];
    v = vars + idx;
    add_var (v->expcopy, inner_most_scope);
  }
  assert (first_clause);
  last = last_clause;
  c = first_clause;
  do { 
    if (c->mark == expansion_cost_mark) 
      expand_clause (c, pivot);
    if (c == last) break;
    c = c->next;
  } while (!empty_clause);
  for (i = 0; i < ncopied; i++) {
    idx = stack[i + 1];
    v = vars + idx;
    assert (v->expcopy == num_vars - ncopied + i + 1);
    v->expcopy = 0;
  }
  expanded++;
  tag_lit (pivot, EXPANDED);
  assert (remaining > 0);
  remaining--;
  LOG ("expanded %d remaining %d", pivot, remaining);
  assign (pivot);
  for (idx = first_new_var; idx <= num_vars; idx++)
    if (null_occurrences (idx)) zombie (idx);
}

static int try_expand (void) {
  int cost, delta, lit, best, min, lim;
  double start, time;
  Scope * p;
  Var * v;
  start = seconds ();
  stretch_scopes ();
  min = lim = (axcess < INT_MAX) ? (axcess + 1) : INT_MAX;
  best = 0;
  for (p = inner_most_scope; p; p = p->outer) {
    if (p->type > 0) continue;
    for (v = p->first; v; v = v->next) {
      if (v->tag != FREE) continue;
      lit = v - vars;
      cost = expand_cost (lit, min);
      if (cost == INT_MAX) continue;
      if (cost >= min) continue;
      best = lit;
      min = cost;
    }
  }
  assert (min == lim || best);
  LOG ("minimial expansion cost is at most %d expanding %d", min, best);
  time = seconds () - start;
  if (min > axcess) {
    msg ("minimial expansion cost limit of %d exceeded in %.1f seconds",
         axcess, time);
    return 0;
  }
  msg ("found minimial expansion cost of %d in %.1f seconds", min, time);
  delta = num_clauses;
  expand (best, min);
  flush (0);
  delta -= num_clauses;
  LOG ("expansion of %d removed %d clauses", best, delta);
  return 1;
}

/* splitting a clause of length C into N clauses of at most length L 
 * by introducing V = N-1 variables:
 *                                                               C/L  N
 *                                                               
 * (1 2 3 4)     -> (1 2 -5)(5 3 4)                              4/3  2
 * (1 2 3 4 5)   -> (1 2 -6)(6 3 -7)(7 4 5)                      5/3  3
 * (1 2 3 4 5 6) -> (1 2 -7)(7 3 -8)(8 4 -9)(9 5 6)              6/3  4
 *                                                               
 *                                                               C/3  C-2
 *                                                         
 * (1 2 3 4 5)         -> (1 2 3 -6)(6 4 5)                      5/4  2
 * (1 2 3 4 5 6)       -> (1 2 3 -7)(7 4 5 6)                    6/4  2
 * (1 2 3 4 5 6 7)     -> (1 2 3 -8)(8 4 5 -9)(9 6 7)            7/4  3
 * (1 2 3 4 5 6 7 8)   -> (1 2 3 -9)(9 4 5 -10)(10 6 7 8)        8/4  3
 * (1 2 3 4 5 6 7 8 9) -> (1 2 3 -a)(a 4 5 -b)(b 6 7 -c)(c 8 9)  9/4  4 
 *
 *                                                               C/4  (C-1)/2
 *
 * (1 2 3 4 5 6)       -> (1 2 3 4 -a)(a 5 6)                    6/5  2
 * (1 2 3 4 5 6 7)     -> (1 2 3 4 -a)(a 5 6 7)                  7/5  2
 * (1 2 3 4 5 6 7 8)   -> (1 2 3 4 -a)(a 5 6 7 8)                8/5  2
 * (1 2 3 4 5 6 7 8 9) -> (1 2 3 4 -a)(a 5 6 7 b)(-b 8 9)        9/5  3
 *
 * (1 2 3 4 5 6 7 8 9 10) ->
 *   (1 2 3 4 -a)(a 5 6 7 b)(b 8 9 10)                          10/5  3
 *
 * (1 2 3 4 5 6 7 8 9 10 11) ->
 *   (1 2 3 4 -a)(a 5 6 7 -b)(b 8 9 10 11)                      11/5  3
 *
 * (1 2 3 4 5 6 7 8 9 10 11 12) ->
 *   (1 2 3 4 -a)(a 5 6 7 -b)(b 8 9 10 -c)(c 11 12)             12/5  4
 *
 * (1 2 3 4 5 6 7 8 9 10 11 12 13) ->
 *   (1 2 3 4 -a)(a 5 6 7 -b)(b 8 9 10 -c)(c 11 12 13)          13/5  4
 *
 * (1 2 3 4 5 6 7 8 9 10 11 12 13 14) ->
 *   (1 2 3 4 -a)(a 5 6 7 -b)(b 8 9 10 -c)(c 11 12 13 14)       14/5  4
 *
 *                                                               C/5  C/3
 *
 * (1 2 3 4 5 6 7) -> (1 2 3 4 5 -a) (a 6 7)                     7/6  2
 * (1 2 3 4 5 6 7 8) -> (1 2 3 4 5 -a) (a 6 7 8)                 8/6  2
 * (1 2 3 4 5 6 7 8 9) -> (1 2 3 4 5 -a) (a 6 7 8 9)             9/6  2
 * (1 2 3 4 5 6 7 8 9 10) -> (1 2 3 4 5 -a) (a 6 7 8 9 10)      10/6  2
 *
 * (1 2 3 4 5 6 7 8 9 10 11) ->
 *   (1 2 3 4 5 -a)(a 6 7 8 9 -b)(b 10 11)                      11/6  3
 *
 * (1 2 3 4 5 6 7 8 9 10 11 12) ->
 *   (1 2 3 4 5 -a)(a 6 7 8 9 -b)(b 10 11 12)                   12/6  3
 *
 * (1 2 3 4 5 6 7 8 9 10 11 12 13) ->
 *   (1 2 3 4 5 -a)(a 6 7 8 9 -b)(b 10 11 12 13)                13/6  3
 *
 * (1 2 3 4 5 6 7 8 9 10 11 12 13 14) ->
 *   (1 2 3 4 5 -a)(a 6 7 8 9 -b)(b 10 11 12 13 14)             14/6  3
 *
 * (1 2 3 4 5 6 7 8 9 10 11 12 13 14 15) ->
 *   (1 2 3 4 5 -a)(a 6 7 8 9 -b)(b 10 11 12 13 -c)(c 14 15)    15/6  4
 *
 *                                                               C/6  (C+1)/4
 */

static int split_clause (Clause * c, int next) {
  int res = next, i, size;
  Node * p;
  LOGCLAUSE (c, "splitting length %d clause", c->size);
  size = c->size;
  assert (size > splitlim);
  assert (!num_lits);
  p = c->nodes;
  for (i = 0; i < splitlim - 1; i++) push_literal (p++->lit);
  push_literal (-res);
  assert (!trivial_clause ());
  add_clause ();
  size -= splitlim - 1;
  while (size > splitlim - 1) {
    assert (!num_lits);
    push_literal (res);
    for (i = 0; i < splitlim - 2; i++) push_literal (p++->lit);
    push_literal (-++res);
    assert (!trivial_clause ());
    add_clause ();
    size -= splitlim - 2;
  }
  assert (2 <= size && size <= splitlim -1);
  push_literal (res++);
  while (size-- > 0) push_literal (p++->lit);
  assert (!p->lit);
  assert (!trivial_clause ());
  add_clause ();
  return res;
}

static void split (void) {
  int sumvars, sumclauses, splitted, size, maxsize, vars, clauses;
  Clause * p, * next_clause;
  int idx, next_idx;
  if (!first_clause) return;
  sumclauses = sumvars = splitted = 0;
  maxsize = -1;
  for (p = first_clause; p; p = p->next) {
    size = p->size;
    if (size > maxsize) maxsize = size;
    if (size <= splitlim) continue;
    clauses = 1;
    size -= splitlim - 1;
    vars = 1;
    while (size > splitlim - 1) {
      size -= splitlim - 2;
      clauses++;
      vars++;
    }
    assert (2 <= size && size < splitlim);
    clauses++;
    assert (vars == clauses - 1);
    sumclauses += clauses;
    sumvars += vars;
    splitted++;
    LOGCLAUSE (p,
               "using %d variables and %d new clauses to split clause",
               vars, clauses); 
  } 
  if (!splitted) {
    assert (0 <= maxsize && maxsize <= splitlim);
    msg ("largest clause size is %d so no need to split any clause", maxsize);
    return;
  }
  msg ("splitting %d clauses using %d variables into %d clauses",
       splitted, sumvars, sumclauses);
  next_idx = num_vars + 1;
  enlarge_vars (num_vars + sumvars);
  for (idx = next_idx; idx <= num_vars; idx++)
    add_quantifier (idx);
  for (p = first_clause; p; p = next_clause) {
    next_clause = p->next;
    if (p->size <= splitlim) continue;
    next_idx = split_clause (p, next_idx);
    delete_clause (p);
  }
  assert (num_vars + 1 == next_idx);
  flush (0);
}

static void map_vars (void) {
  Var * v;
  mapped = 0;
  assert (trail_flushed ());
  for (v = vars + 1; v <= vars + num_vars; v++) {
    if (v->tag != FREE) continue;
    v->mapped = ++mapped;
    LOG ("mapped %d to %d", (int)(long)(v - vars), mapped);
  }
  if (empty_clause) assert (!mapped), remaining = 0;
  else assert (mapped == remaining);
}

static void print_clause (Clause * c, FILE * file) {
  Node * p;
  for (p = c->nodes; p->lit; p++)
    fprintf (file, "%d ", map_lit (p->lit));
  fputs ("0\n", file);
}

static void print_clauses (FILE * file) {
  Clause * p;
  for (p = first_clause; p; p = p->next)
    print_clause (p, file);
}

static void print_scope (Scope * s, FILE * file) {
  Var * p;
  fputc (s->type < 0 ? 'a' : 'e', file);
  for (p = s->first; p; p = p->next) {
    if (p->tag != FREE) continue;
    fprintf (file, " %d", map_lit (var2lit (p)));
  }
  fputs (" 0\n", file);
}

static int empty_scope (Scope * s) {
  int res = 1;
  Var * p;
  for (p = s->first; res && p; p = p->next)
    if (p->tag == FREE)
      res = 0;
  assert (res == !s->free);
  return res;
}

static int propositional (void) {
  Scope * p;
  for (p = outer_most_scope; p; p = p->inner)
    if (p->type < 0 && !empty_scope (p)) return 0;
  return 1;
}

static void print_scopes (FILE * file) {
  Scope * p, * first = 0;
  if (propositional ()) return;
  for (p = outer_most_scope; p; p = p->inner) {
    if (!quantifyall && !first && p->type > 0) continue;
    if (empty_scope (p)) continue;
    first = p;
    print_scope (p, file);
  }
}

static void print (FILE * file) {
  fprintf (file, "p cnf %d %d\n", mapped, num_clauses);
  print_scopes (file);
  print_clauses (file);
}

static void release_clauses (void) {
  Clause * p, * next;
  size_t bytes;
  for (p = first_clause; p; p = next) {
    next = p->next;
    bytes = bytes_clause (p->size);
    DEC (bytes);
    free (p);
  }
}

static void release_scopes (void) {
  Scope * p, * next;
  for (p = outer_most_scope; p; p = next) {
    next = p->inner;
    DEL (p);
  }
}

static void release (void) {
  release_clauses ();
  release_scopes ();
  DELN (line, szline);
  DELN (lits, size_lits);
  DELN (stack, szstack);
  DELN (aux, szaux);
  DELN (vars, num_vars + 1);
  dfsi -= num_vars;
  DELN (dfsi, 2*num_vars+1);
  mindfsi -= num_vars;
  DELN (mindfsi, 2*num_vars+1);
  repr -= num_vars;
  DELN (repr, 2*num_vars+1);
  DELN (bwsigs, num_vars + 1);
  DELN (fwsigs, num_vars + 1);
  DELN (anchors, num_vars + 1);
  DELN (trail, num_vars);
  DELN (schedule, num_vars);
  assert (getenv ("LEAK") || current_bytes == 0);
}

static double percent (double a, double b) {
  return b ? (100.0 * a / b) : 0.0;
}

static double average (double a, double b) {
  return b ? a / b : 0.0;
}

static const char * USAGE =
"usage: bloqqer [<option> ...] [<in> [<out>]]\n"
"\n"
"input and output files are specified as (use '-' for default)\n"
"\n"
"  <in>                input QDIMACS file (default <stdin>)\n"
"  <out>               output QDIMACS file (default <stdout>)\n"
"\n"
"  -n                  no output, see also '--output' below\n"
"\n"
"For all of the following options '--<opt>=<v>' there also exist an\n"
"'--no-<opt>' and '--<opt>' version.  The former sets the option value\n"
"to zero and the later increments the current / default value.  They\n"
"can also be embedded in comments before the 'p cnf' header in '<in>'.\n"
"\n"
;

static void init_opts (void) {
  Opt * p;
  for (p = opts; p->name; p++) {
    assert (p->low <= p->def && p->def <= p->high);
    *p->valptr = p->def;
  }
  if (bound >= 0) force_bound ();
}

static void list_usage (void) {
  int len, tmp, i;
  char * buf;
  Opt * p;
  fputs (USAGE, stdout);
  len = 0;
  for (p = opts; p->name; p++) {
    tmp = strlen (p->name) + 2;
    if (p->inc) tmp += 3;
    if (tmp > len) len = tmp;
  }
  len += 8;
  NEWN (buf, len + 1);
  for (p = opts; p->name; p++) {
    if (p->inc) sprintf (buf, "-%c | --%s=<v>", p->inc, p->name);
    else sprintf (buf, "     --%s=<v>", p->name);
    assert (strlen (buf) <= len);
    fputs (buf, stdout);
    for (i = strlen (buf); i < len; i++) fputc (' ', stdout);
    fputs (p->description, stdout);
    printf (" (default %d)\n", p->def);
  }
  DELN (buf, len + 1);
}

static void list_opts_ranges (void) {
  Opt * p = opts;
  while (strcmp (p->name, "range")) p++;
  for (p++; p->name; p++)
    printf ("%s %d %d %d\n", p->name, p->def, p->low, p->high);
}

static void list_opts_defaults (void) {
  Opt * p;
  for (p = opts; p->name; p++)
    printf ("%s %d\n", p->name, p->def);
}

static void list_opts_defaults_in_embedded_format (void) {
  Opt * p = opts;
  while (strcmp (p->name, "range")) p++;
  for (p++; p->name; p++)
    printf ("c --%s=%d\n", p->name, p->def);
}

static void stats (void) {
  msg ("");
  msg ("[final statistics follow]");
  msg ("");
  msg ("%d units %.0f%%", units, percent (units, num_vars));
  msg ("%d zombie variables %.0f%%", zombies, percent (zombies, num_vars));
  msg ("%d eliminated variables %.0f%%", 
       eliminated, percent (eliminated, num_vars));
  msg ("%d expanded variables %.0f%%", 
       expanded, percent (expanded, num_vars));
  msg ("%d substituted variables %.0f%%", 
       substituted, percent (substituted, num_vars));
  msg ("%d existential pure literals %.0f%%", 
       unets, percent (unets, num_vars));
  msg ("%d universal pure literals %.0f%%", 
       unates, percent (unates, num_vars));
  msg ("");
  msg ("%d pushed variables %.2f per variable", 
       pushed_vars, average (pushed_vars, num_vars));
  msg ("%d added clauses %.2f per original clause",
       added_clauses, average (added_clauses, orig_clauses));
  msg ("%d enqueued clauses %.2f per added clause", 
       enqueued_clauses, average (enqueued_clauses, added_clauses));
  msg ("");
  msg ("%d subsumed clauses (%.0f%% of all added clauses)", 
       subsumed_clauses, percent (subsumed_clauses, added_clauses));
  msg ("%d backward subsumed (%.0f%% of all subsumed clauses)",
       backward_subsumed_clauses,
       percent (backward_subsumed_clauses, subsumed_clauses));
  msg ("%d forward subsumed (%.0f%% of all subsumed clauses)",
       forward_subsumed_clauses,
       percent (forward_subsumed_clauses, subsumed_clauses));
  msg ("");
  msg ("%d strengthened clauses (%.0f%% of all added clauses)", 
       strengthened_clauses, percent (strengthened_clauses, added_clauses));
  msg ("%d backward strengthened (%.0f%% of all strengthened clauses)",
       backward_strengthened_clauses,
       percent (backward_strengthened_clauses, strengthened_clauses));
  msg ("%d forward strengthened (%.0f%% of all strengthened clauses)",
       forward_strengthened_clauses,
       percent (forward_strengthened_clauses, strengthened_clauses));
  msg ("");
  msg ("%lld fwsig1 lookups with %lld hits (%.0f%% hit rate)",
       fw.sig1.lookups, fw.sig1.hits, percent (fw.sig1.hits, fw.sig1.lookups));
  msg ("%lld fwsig2 lookups with %lld hits (%.0f%% hit rate)",
       fw.sig2.lookups, fw.sig2.hits, percent (fw.sig2.hits, fw.sig2.lookups));
  msg ("%lld bwsig1 lookups with %lld hits (%.0f%% hit rate)",
       bw.sig1.lookups, bw.sig1.hits, percent (bw.sig1.hits, bw.sig1.lookups));
  msg ("%lld bwsig2 lookups with %lld hits (%.0f%% hit rate)",
       bw.sig2.lookups, bw.sig2.hits, percent (bw.sig2.hits, bw.sig2.lookups));
  msg ("");
  msg ("%d equivalence reasoning rounds", eqrounds);
  msg ("%lld hidden %lld covered literal additions", hlas, clas);
  msg ("");
  msg ("%d blocked clauses %.0f%% of all added clauses", 
       blocked_clauses, percent (blocked_clauses, added_clauses));
  msg ("%d hidden tautologies %.0f%% of all added clauses", 
       hidden_tautologies, percent (hidden_tautologies, added_clauses));
  msg ("%d hidden blocked clauses %.0f%% of all added clauses", 
       hidden_blocked_clauses,
       percent (hidden_blocked_clauses, added_clauses));
  msg ("%d non-strict variable eliminations %.0f%%",
       nonstrictves, percent (nonstrictves, eliminated));
  msg ("");
  msg ("%d remaining variables %.0f%% out of %d", 
       remaining, percent (remaining, num_vars), num_vars);
  msg ("%d remaining clauses %.0f%% out of %d", 
       num_clauses, percent (num_clauses, orig_clauses), orig_clauses);
  msg ("");
  msg ("%.3f seconds, %.1f MB", seconds (), max_bytes /(double)(1<<20));
}

int main (int argc, char ** argv) {
  const char * perr;
  int i, res;
  init_opts ();
  for (i = 1; i < argc; i++) if (!strcmp (argv[i], "-v")) verbose = 1;
  msg ("Bloqqer QBF Preprocessor");
  msg ("Version %s %s", blqr_version (), blqr_id ());
  msg ("Copyright (C) 2010 by Armin Biere JKU Linz");
  msg ("%s", blqr_cflags());
  for (i = 1; i < argc; i++) {
    if (parse_opt (argv[i])) {
      if (help) { list_usage (); exit (0); }
      if (range) { list_opts_ranges (); exit (0); }
      if (defaults) { list_opts_defaults (); exit (0); }
      if (embedded) { list_opts_defaults_in_embedded_format (); exit (0); }
      command_line_options++;
      msg ("command line option: %s", argv[i]);
    } else if (argv[i][0] == '-' && argv[i][1]) 
      die ("invalid command line option '%s' (try '-h')", argv[i]);
    else if (oname) die ("too many file names (try '-h')");
    else if (iname) oname = argv[i], msg ("output: %s", oname);
    else iname = argv[i], msg ("input: %s", iname);
  }
  if (oname && !output) die ("both '-n' and '<out>' specified");
  if (iname && strcmp (iname, "-")) {
    if ((i = strlen (iname)) >= 3 && !strcmp (iname + i - 3, ".gz")) {
      char * cmd;
      NEWN (cmd, i + 30);
      sprintf (cmd, "gunzip -c %s", iname);
      ifile = popen (cmd, "r");
      DELN (cmd, i + 30);
      ipclose = 1;
    } else {
      ifile = fopen (iname, "r");
      ifclose = 1;
    }
    if (!ifile) die ("can not read '%s'", iname);
  } else {
    ifile = stdin;
    iname = "<stdin>";
  }
  terminal = isatty (1);
  msg ("finished parsing %d command line options", command_line_options);
  perr = parse ();
  if (perr) {
    stop_progress ();
    fprintf (stderr, "%s:%d: %s\n", iname, lineno, perr);
    fflush (stderr);
    exit (1);
  }
  if (ifclose) fclose (ifile);
  if (ipclose) pclose (ifile);
  flush_vars ();

  if(expand_variable) {
    apply_expansion_config();
    flush(1);
    // This if is required, as otherwise trivial formulas could not be
    // expanded!
    if(!empty_clause && num_clauses) {
      int cost = expand_cost(expand_variable, IM);
      if(cost < maxexpvarcost) {
        expand(expand_variable, cost);
        res = 1;
      } else {
        res = 2;
      }
    }
  }

  for (;;) {
    flush (1);
    split ();
    if (empty_clause || !num_clauses) break;
    if (eqres (1)) flush (0);
    if (empty_clause || !num_clauses) break;
    elim ();
    if (verbose) log_pruned_scopes ();
    if (empty_clause || !num_clauses) break;
    if (propositional ()) break;
    if (!try_expand ()) break;
  }
  
  if (empty_clause) { res = 20; msg ("definitely UNSATISFIABLE"); }
  else if (!num_clauses) { res = 10; msg ("definitely SATISFIABLE"); }
  split ();
  if (keep) remaining = num_vars; else map_vars ();
  if (oname && strcmp (oname, "-")) {
    assert (output);
    ofile = fopen (oname, "w");
    if (!ofile) die ("can not write '%s'", oname);
    oclose = 1;
  } else if (output) {
    ofile = stdout;
    oname = "<stdout>";
  }
  if (propositional ()) msg ("result is propositional");
  else msg ("result still contains universal quantifiers");
  if (output) print (ofile);
  if (oclose) fclose (ofile);
  release ();
  stats ();
  return res;
}

#ifndef NDEBUG
void dump (void) { print (stdout); }
void dump_clause (Clause * c) { print_clause (c, stdout); }
void dump_scope (Scope * s) { print_scope (s, stdout); }
#endif
