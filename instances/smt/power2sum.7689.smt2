(set-info :smt-lib-version 2.6)
(set-logic QF_BV)
(set-info :source |
We verify that the sum of two distinct powers of two cannot be a power of two.
Contributed by Andreas Froehlich, Gergely Kovasznai, Armin Biere
Institute for Formal Models and Verification, JKU, Linz, 2013
source: http://fmv.jku.at/smtbench and "Efficiently Solving Bit-Vector Problems Using Model Checkers" by Andreas Froehlich, Gergely Kovasznai, Armin Biere. In Proc. 11th Intl. Workshop on Satisfiability Modulo Theories (SMT'13), pages 6-15, aff. to SAT'13, Helsinki, Finland, 2013.
|)
(set-info :category "crafted")
(set-info :status unsat)
(declare-fun x () (_ BitVec 7689))
(declare-fun y () (_ BitVec 7689))
(declare-fun z () (_ BitVec 7689))
(assert (= z (bvadd x y)))
(assert (distinct x y))
(assert (and (distinct x (_ bv0 7689)) (= (bvand x (bvsub x (_ bv1 7689))) (_ bv0 7689))))
(assert (and (distinct y (_ bv0 7689)) (= (bvand y (bvsub y (_ bv1 7689))) (_ bv0 7689))))
(assert (and (distinct z (_ bv0 7689)) (= (bvand z (bvsub z (_ bv1 7689))) (_ bv0 7689))))
(check-sat)
(exit)
