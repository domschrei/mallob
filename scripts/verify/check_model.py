#!/usr/bin/env python3
"""
Verify that a satisfying assignment satisfies a DIMACS CNF formula.

Usage:
    python verify_cnf.py <cnf_file> <assignment_file>
"""

import sys


def parse_cnf(path):
    """Parse a DIMACS CNF file, returning (num_vars, num_clauses, clauses)."""
    clauses = []
    current_clause = []
    num_vars = None
    num_clauses = None

    with open(path, "r") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("c"):
                continue  # skip blank lines and comments
            if line.startswith("p"):
                parts = line.split()
                # p cnf <num_vars> <num_clauses>
                num_vars = int(parts[2])
                num_clauses = int(parts[3])
                continue

            for tok in line.split():
                lit = int(tok)
                if lit == 0:
                    if current_clause:
                        clauses.append(current_clause)
                        current_clause = []
                else:
                    current_clause.append(lit)

    if current_clause:
        clauses.append(current_clause)

    if num_vars is None:
        raise ValueError("No problem line ('p cnf ...') found in CNF file")
    if num_clauses is not None and len(clauses) != num_clauses:
        print(
            f"Warning: header declares {num_clauses} clauses, "
            f"but {len(clauses)} were parsed.",
            file=sys.stderr,
        )

    return num_vars, num_clauses, clauses


def parse_assignment(path):
    """Parse a zero-terminated, whitespace-separated literal assignment."""
    for line in open(path, "r").readlines():
        a = parse_assignment_line(line.rstrip())
        if a:
            return a

def parse_assignment_line(text):

    if not text.startswith("v"):
        return None

    tokens = text.split()[1:]
    literals = [int(tok) for tok in tokens]

    if not literals or literals[-1] != 0:
        raise ValueError("Assignment must be zero-terminated")

    literals = literals[:-1]  # drop trailing 0

    assignment = {}
    for lit in literals:
        if lit == 0:
            raise ValueError("Unexpected 0 in the middle of the assignment")
        var = abs(lit)
        value = lit > 0
        if var in assignment and assignment[var] != value:
            raise ValueError(f"Conflicting assignment for variable {var}")
        assignment[var] = value

    return assignment


def check_assignment(clauses, assignment):
    """Return (satisfied, list_of_unsatisfied_clause_indices)."""
    unsatisfied = []

    for idx, clause in enumerate(clauses, start=1):
        clause_satisfied = False
        for lit in clause:
            var = abs(lit)
            value = assignment.get(var)
            if value is None:
                # Unassigned variable: treat as not making this literal true.
                continue
            lit_is_true = (lit > 0 and value) or (lit < 0 and not value)
            if lit_is_true:
                clause_satisfied = True
                break
        if not clause_satisfied:
            unsatisfied.append(idx)

    return (len(unsatisfied) == 0), unsatisfied


def main():
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} <cnf_file> <assignment_file>")
        sys.exit(1)

    cnf_path, assignment_path = sys.argv[1], sys.argv[2]

    num_vars, num_clauses, clauses = parse_cnf(cnf_path)
    assignment = parse_assignment(assignment_path)

    # Sanity check: assignment shouldn't reference variables outside range
    out_of_range = [v for v in assignment if v < 1 or v > num_vars]
    if out_of_range:
        print(
            f"Warning: assignment references variables outside declared range "
            f"(1..{num_vars}): {out_of_range}",
            file=sys.stderr,
        )

    satisfied, unsatisfied = check_assignment(clauses, assignment)

    print(f"CNF file: {cnf_path}")
    print(f"Variables declared: {num_vars}, Clauses parsed: {len(clauses)}")
    print(f"Assignment file: {assignment_path} ({len(assignment)} variables assigned)")
    print()

    if satisfied:
        print("RESULT: Assignment SATISFIES the formula. ✅")
    else:
        print("RESULT: Assignment does NOT satisfy the formula. ❌")
        print(f"Unsatisfied clause(s) ({len(unsatisfied)} of {len(clauses)}):")
        for idx in unsatisfied[:20]:
            print(f"  Clause {idx}: {clauses[idx - 1]}")
        if len(unsatisfied) > 20:
            print(f"  ... and {len(unsatisfied) - 20} more.")

    sys.exit(0 if satisfied else 1)


if __name__ == "__main__":
    main()

