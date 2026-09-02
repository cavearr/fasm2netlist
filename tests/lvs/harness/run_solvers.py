#!/usr/bin/env python3
"""Run one DIMACS CNF through several SAT solvers and compare verdict + time."""
import sys, time
from pysat.formula import CNF
from pysat.solvers import Solver

cnf = CNF(from_file=sys.argv[1])
print('CNF: %d variables, %d clauses' % (cnf.nv, len(cnf.clauses)))
names = ['minisat22', 'glucose42', 'cadical153', 'cadical195', 'kissat404', 'cryptominisat', 'lingeling']
for n in names:
    try:
        t = time.time()
        with Solver(name=n, bootstrap_with=cnf.clauses) as s:
            r = s.solve()
        print('  %-14s %-6s %6.2fs' % (n, 'SAT' if r else 'UNSAT', time.time() - t))
    except Exception as e:
        print('  %-14s unavailable (%s)' % (n, type(e).__name__))
