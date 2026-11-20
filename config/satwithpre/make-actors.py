#!/usr/bin/env python3
"""Generate an actor-chain .json file from a short chain string.

Provide a string of the desired chain(s).

Examples
--------
    python3 make_actors.py KM             # -> gen_actors_KM.json
    python3 make_actors.py M_SM_SKM       # -> gen_actors_M_SM_SKM.json

Availale Actors:
    M  = MALLOBSAT (with default -satsolver= options from the $preset in sbatch)
    Mf = MALLOBSAT with "options": "-satsolver=k"    ("full")
    Mp = MALLOBSAT with "options": "-satsolver=k_"   ("plain")
    E = MALLOBSWEEP 
    S = SATSUMA_EXT   
    K = KISSAT
    L = LINGELING

Chains can be separated by '_', and shared prefixes use the same predecessor actor

By default, the last actor of each chain displaces the last actors of all earlier chains 
(except its own ancestors) and gets "onlyStartIfPrerequisiteSimplified": true. 
S, K and L never displace and are never displaced, since they are single-core
"""

import argparse
import json
import sys
 
# token -> (actor type, built-in options or None)
TOKENS = {
    "M":   ("MALLOBSAT",   None),
    "Mf":  ("MALLOBSAT",   "-satsolver=k"),    # "full"
    "Mp":  ("MALLOBSAT",   "-satsolver=k_"),   # "plain"
    "S":   ("SATSUMA_EXT", None),
    "K":   ("KISSAT",      None),
    "E":   ("MALLOBSWEEP", None),
    "L":   ("LINGELING",   None),
}
 
# single-core actors: they never displace others and are never displaced
SINGLE_CORE = {"S", "K", "L"}
 
 
def parse_spec(spec):
    """Split a spec on '_' into chains, each a list of tokens (longest match)."""
    by_len = sorted(TOKENS, key=len, reverse=True)
    chains = []
    for word in spec.split("_"):
        cur, i = [], 0
        while i < len(word):
            tok = next((t for t in by_len if word.startswith(t, i)), None)
            if tok is None:
                raise ValueError(f"unknown actor letter '{word[i]}' in chain '{word}' "
                                 f"(known: {', '.join(TOKENS)})")
            cur.append(tok)
            i += len(tok)
        if "L" in cur[:-1]:
            raise ValueError(f"chain '{word}': nothing may follow L (Lingeling "
                             f"must be the last actor of a chain)")
        if cur:
            chains.append(cur)
    if not chains:
        raise ValueError("empty spec")
    return chains
 
 
def build_actors(spec, options=None, groups=None, no_displace=False):
    options = options or {}
    groups = groups or {}
 
    actors = {}          # id -> dict, insertion-ordered
    terminals = []       # token tuples of the last actor of each chain so far
 
    for chain in parse_spec(spec.strip()):
        for i in range(1, len(chain) + 1):
            prefix = tuple(chain[:i])
            aid = "".join(prefix)
            if aid in actors:
                continue
            displaced = []
            if (i == len(chain) and not no_displace
                    and chain[-1] not in SINGLE_CORE):
                # displace earlier chain ends, but never our own ancestors
                # and never S/K/L actors
                displaced = ["".join(t) for t in terminals
                             if prefix[:len(t)] != t
                             and t[-1] not in SINGLE_CORE]
            atype, builtin = TOKENS[chain[i - 1]]
            actor = {
                "id": aid,
                "type": atype,
                "prerequisite": "".join(chain[:i - 1]) or None,
                "actorsBeingDisplaced": displaced,
                "onlyStartIfPrerequisiteSimplified": bool(displaced),
            }
            opts = " ".join(o for o in (builtin, options.get(aid)) if o)
            if opts:
                actor["options"] = opts
            if aid in groups:
                actor["group-id"] = groups[aid]
            actors[aid] = actor
        if tuple(chain) not in terminals:
            terminals.append(tuple(chain))
 
    unknown = (set(options) | set(groups)) - set(actors)
    if unknown:
        raise ValueError(f"--opt/--group refer to unknown actor id(s): {sorted(unknown)}")
 
    return list(actors.values())
 
 
def to_json(actors):
    """Format like the hand-written files: 4-space indent, inline lists."""
    blocks = []
    for a in actors:
        lines = [f"        {json.dumps(k)}: {json.dumps(v)}" for k, v in a.items()]
        blocks.append("    {\n" + ",\n".join(lines) + "\n    }")
    return "[\n" + ",\n".join(blocks) + "\n]\n"
 
 
def parse_kv(pairs, flag):
    out = {}
    for p in pairs or []:
        if "=" not in p:
            raise ValueError(f"{flag} expects ID=VALUE, got '{p}'")
        k, v = p.split("=", 1)
        out[k] = v
    return out
 
 
def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("spec", help="actor spec, e.g. KM, M_KM, M_SM_SKM")
    ap.add_argument("-o", "--output",
                    help="output file (default: gen_actors_<spec>.json, '-' for stdout)")
    ap.add_argument("--opt", action="append", metavar="ID=OPTIONS",
                    help="set \"options\" for an actor, e.g. --opt SKM='-satsolver=k'")
    ap.add_argument("--group", action="append", metavar="ID=GROUP",
                    help="set \"group-id\" for an actor, e.g. --group SKE=1")
    ap.add_argument("--no-displace", action="store_true",
                    help="never displace actors (all lists empty, all flags false)")
    args = ap.parse_args()
 
    try:
        actors = build_actors(args.spec,
                              parse_kv(args.opt, "--opt"),
                              parse_kv(args.group, "--group"),
                              args.no_displace)
    except ValueError as e:
        ap.error(str(e))
 
    text = to_json(actors)
    out = args.output or f"gen_actors_{args.spec}.json"
    if out == "-":
        sys.stdout.write(text)
    else:
        with open(out, "w") as f:
            f.write(text)
        print(f"wrote {out} ({len(actors)} actors)", file=sys.stderr)
 
 
if __name__ == "__main__":
    main()
