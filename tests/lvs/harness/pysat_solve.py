#!/home/jonathan/xc7-bitstream-tools/.venv/bin/python
# DIMACS-in, SAT-competition-answer-out wrapper so any pysat solver can be
# used as a plain command, which is all lvs_equiv's --solver expects.
import sys
from pysat.formula import CNF
from pysat.solvers import Solver
name = sys.argv[1]
cnf = CNF(from_file=sys.argv[2])
with Solver(name=name, bootstrap_with=cnf.clauses) as s:
    print("s SATISFIABLE" if s.solve() else "s UNSATISFIABLE")
