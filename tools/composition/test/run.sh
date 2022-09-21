#!/bin/bash


#keep a count of how many tests fail and how many are run
num_tests=0
failed_tests=0


#define function for running test
function run_test(){
    #arguments:
    #  1. DIMACS file
    #  2. name for output LRAT file
    #  3. expected return value from composition
    #  4--?. FRAT files to read in order
    num_tests=$(($num_tests + 1))
    echo ""
    echo ""
    echo "../build/compose-proofs $1 $2 ${@:4}"
    ../build/compose-proofs $1 $2 ${@:4}
    result=$?
    if [ ! "$result" -eq "$3" ]; then
        echo "----------------------------------------------------------------------"
        echo "FAILED TEST:  returned $result but expected $3"
        echo "----------------------------------------------------------------------"
        failed_tests=$(($failed_tests + 1))
    elif [ "$result" -eq 0 ]; then
        #test passed and the expected result was success
        lrat-check $1 $2
        lrat_result=$?
        if [ ! "$lrat_result" -eq 0 ]; then
            #test failed by bad LRAT check
            echo "----------------------------------------------------------------------"
            echo "FAILED TEST:  lrat-check failed to verify output"
            echo "----------------------------------------------------------------------"
            failed_tests=$(($failed_tests + 1))
        fi
        #remove file created by the test
    fi
    cat $2
    rm $2
}


#run the tests

#successes
run_test 2color_map/2color_map.dimacs 2color_map/out.lrat 0 2color_map/out1.frat 2color_map/out2.frat 2color_map/out3.frat
run_test lrat_paper/lrat_paper.dimacs lrat_paper/out.lrat 0 lrat_paper/out1.frat lrat_paper/out2.frat
run_test extra_clauses/extra.dimacs extra_clauses/out.lrat 0 extra_clauses/out1.frat
run_test incomplete_clauses/2color_map.dimacs incomplete_clauses/out.lrat 0 incomplete_clauses/out1.frat incomplete_clauses/out2.frat incomplete_clauses/out3.frat

#failure because of clauses coming out of wrong instance files
run_test lrat_paper/lrat_paper.dimacs lrat_paper/out.lrat 12 lrat_paper/out2.frat lrat_paper/out1.frat

#nonexistent files
run_test no_file.dimacs lrat_paper/out.lrat 5 lrat_paper/out1.frat lrat_paper/out2.frat
run_test lrat_paper/lrat_paper.dimacs lrat_paper/out.lrat 5 lrat_paper/out1.frat lrat_paper/out2.frat lrat_paper/out3.frat

#no empty clause
run_test no_empty_clause/lrat_paper.dimacs no_empty_clause/out.lrat 8 no_empty_clause/out1.frat no_empty_clause/out2.frat


#report final results
echo ""
echo ""
if [ $failed_tests -eq 0 ]; then
    echo "======================================================================"
    echo "All tests passed."
    echo "======================================================================"
    exit 0
else
    echo "======================================================================"
    echo "$failed_tests of $num_tests failed."
    echo "======================================================================"
    exit 1
fi

