
#include "app/maxsat/encoding/cardinality_encoding.hpp"
#include "app/maxsat/encoding/generalized_totalizer.hpp"
#include "app/maxsat/encoding/polynomial_watchdog.hpp"
#include "app/maxsat/encoding/warners_adder.hpp"
#include "app/maxsat/maxsat_instance.hpp"
#include "app/maxsat/parse/maxsat_reader.hpp"
#include "app/sat/parse/sat_reader.hpp"
#include "app/sat/solvers/kissat.hpp"
#include "data/job_description.hpp"
#include "interface/api/api_connector.hpp"
#include "util/logger.hpp"
#include "util/params.hpp"
#include "util/random.hpp"
#include "util/string_utils.hpp"
#include "util/sys/fileutils.hpp"
#include "util/sys/process.hpp"
#include "util/sys/thread_pool.hpp"
#include "util/sys/timer.hpp"
#include <cstdint>
#include <fstream>
#include <initializer_list>
#include <vector>

int newVar(int& nbVars) {
    nbVars++;
    return nbVars;
}

void trivialTest() {
    std::vector<MaxSatInstance::ObjectiveTerm> objective {{1, 1}, {1, 2}};
    int nbVars = 2;
    int nbClauses = 0;
    std::vector<int> lits;
    PolynomialWatchdog enc(nbVars, objective);
    std::vector<int> assumptions;
    enc.setClauseCollector([&](int lit) {
        lits.push_back(lit);
        nbClauses += lit==0;
    });
    enc.setAssumptionCollector([&](int lit) {
        assumptions.push_back(lit);
    });

    enc.encode(0, 0, 2);
    enc.encode(0, 1, 2);
    LOG(V2_INFO, "FORMULA %s\n", StringUtils::getSummary(lits, INT32_MAX).c_str());
    LOG(V2_INFO, "ASSUMPTIONS %s\n", StringUtils::getSummary(assumptions, INT32_MAX).c_str());

    SolverSetup setup;
    setup.logger = &Logger::getMainInstance();
    setup.numVars = nbVars;
    setup.numOriginalClauses = nbClauses;
    Kissat kissat(setup);
    for (int lit : lits) kissat.addLiteral(lit);
    kissat.addLiteral(1);
    kissat.addLiteral(0);
    kissat.addLiteral(2);
    kissat.addLiteral(0);
    int res = kissat.solve(0, 0);
    assert(res == 10);
}

void encode(Parameters& params, const std::string& path, const std::string& filename) {

    JobDescription desc;
    desc.setRevision(0);
    MaxSatReader reader(params, path + "/" + filename);
    bool ok = reader.read(desc);
    assert(ok);
    LOG(V2_INFO, "Formula has size %i, %i variables, %i clauses\n", desc.getFSize(), reader.getNbVars(), reader.getNbClauses());

    // Re-map variables in-place to a compact domain from 1 to |O|
    int nbBaseVars = 0;
    MaxSatInstance instance(desc, false, params.maxSatIntervalSkew());
    for (auto& [weight, lit] : instance.objective) {
        lit = (lit>0 ? 1 : -1) * newVar(nbBaseVars);
    }

    instance.print(0);
    LOG(V2_INFO, "Max. bound: %lu\n", instance.upperBound);

    // Initialize encoders (lazily so that they can use different auxiliary variables)
    int nbVars;
    std::vector<std::pair<std::string, std::function<CardinalityEncoding*()>>> encs = {
        {"dpw", [&]() {return new PolynomialWatchdog(nbVars, instance.objective);}},
        {"gte", [&]() {return new GeneralizedTotalizer(nbVars, instance.objective);}},
        {"add", [&]() {return new WarnersAdder(nbVars, instance.objective);}},
    };
    // Generate all encoder pairs
    std::vector<std::pair<int, int>> pairs;
    for (size_t i = 0; i < encs.size(); i++)
        for (size_t j = i+1; j < encs.size(); j++)
            pairs.push_back({i,j});

    // For each pair
    for (auto [i,j] : pairs) {

        std::vector<int> formula;
        int nbClauses {0};
        std::vector<int> outputVars;

        // Allocate one-hot variables for all possible bounds
        nbVars = nbBaseVars;
        tsl::robin_map<size_t, int> boundVars;
        size_t max = instance.upperBound;
        size_t maxEncoded = max;
        for (size_t bound = 0; bound <= maxEncoded; bound++) {
            boundVars[bound] = newVar(nbVars);
        }

        // For each encoding in the pair
        for (int k : {i, j}) {
            auto& [label, encfunc] = encs[k];
            int encOutputVar = newVar(nbVars);
            outputVars.push_back(encOutputVar);
            auto enc = encfunc();

            size_t bound;
            tsl::robin_map<size_t, std::vector<int>> boundToAssumptions;
            std::vector<int> pbLiterals;
            std::vector<int> clauseVars;
            enc->setClauseCollector([&](int lit) {
                pbLiterals.push_back(lit);
                nbVars = std::max(nbVars, std::abs(lit));
            });
            enc->setAssumptionCollector([&](int lit) {
                boundToAssumptions[bound].push_back(lit);
            });

            // Encode the actual PB constraints
            for (int i = maxEncoded; i >= 0; i--) {
                bound = i;
                enc->encode(0, bound, max);
            }
            // Encode every single bound enforcement, remembering the respective assumptions
            for (int i = maxEncoded; i >= 0; i--) {
                bound = i;
                enc->enforceBound(bound);
            }
            nbVars = std::max(nbVars, enc->getNbVars());

            // Transform the arising encoding, making it "relative" to an output variable
            std::vector<int> begunClause;
            std::string writtenEnc;
            for (int lit : pbLiterals) {
                writtenEnc += std::to_string(lit) + " ";
                if (lit != 0) begunClause.push_back(lit);
                else {
                    writtenEnc += "\n";
                    // Allocate variable to represent this clause
                    int clauseOutputVar = newVar(nbVars);
                    clauseVars.push_back(clauseOutputVar);
                    // If the clause is true, some literal must be true
                    formula.push_back(-clauseOutputVar);
                    for (int lit : begunClause) {
                        formula.push_back(lit);
                    }
                    formula.push_back(0);
                    nbClauses++;
                    // If the clause is false, each literal must be false
                    for (int lit : begunClause) {
                        formula.push_back(clauseOutputVar);
                        formula.push_back(-lit);
                        formula.push_back(0);
                        nbClauses++;
                    }
                    // If the encoding outputs true, the clause must be true
                    formula.push_back(-encOutputVar);
                    formula.push_back(clauseOutputVar);
                    formula.push_back(0);
                    nbClauses++;
                    // done
                    begunClause.clear();
                }
            }
            printf("ENC:\n%s", writtenEnc.c_str());

            // If the encoding outputs false, some clause must be false
            formula.push_back(encOutputVar);
            for (int v : clauseVars) formula.push_back(-v);
            formula.push_back(0);
            nbClauses++;

            for (auto& [bound, asmpt] : boundToAssumptions) {
                // The bound variable needs to imply each of the assumptions
                for (int l : asmpt) {
                    formula.push_back(-boundVars.at(bound));
                    formula.push_back(l);
                    formula.push_back(0);
                    nbClauses++;
                }
                // If all assumptions are set, the bound variable must hold
                for (int l : asmpt) formula.push_back(-l);
                formula.push_back(boundVars.at(bound));
                formula.push_back(0);
                nbClauses++;
            }

            delete enc;
        }

        // The two encoder's outputs shall differ
        assert(outputVars.size() == 2);
        formula.push_back(-outputVars[0]);
        formula.push_back(-outputVars[1]);
        formula.push_back(0);
        nbClauses++;
        formula.push_back(outputVars[0]);
        formula.push_back(outputVars[1]);
        formula.push_back(0);
        nbClauses++;

        // Some bound variable must be set
        for (auto& [bound, var] : boundVars) {
            formula.push_back(var);
        }
        formula.push_back(0);
        nbClauses++;

        auto outpath = ".tmp/pbenc-equiv-check-" + encs[i].first + "-vs-" + encs[j].first + "-" + filename + ".cnf";
        {
            std::ofstream cnfOut(outpath);
            cnfOut << "p cnf " << nbVars << " " << nbClauses << "\n";
            for (int lit : formula) {
                cnfOut << lit << " ";
                if (lit==0) cnfOut << "\n";
            }
            //cnfOut << assumptions.back() << " 0\n";
        }
        LOG(V2_INFO, "Formula written to %s\n", outpath.c_str());

        SolverSetup setup;
        setup.logger = &Logger::getMainInstance();
        setup.numVars = nbVars;
        setup.numOriginalClauses = nbClauses;
        Kissat kissat(setup);
        for (int lit : formula) kissat.addLiteral(lit);
        int res = kissat.solve(0, 0);
        LOG(V2_INFO, "Kissat returned %i\n", res);
        if (res == 10) {
            std::vector<int> model = kissat.getSolution();
            for (auto [bound, boundVar] : boundVars) {
                assert(boundVar > 0);
                assert(model.size() > boundVar);
                int solLit = model[boundVar];
                if (solLit > 0) LOG(V2_INFO, " -- bound <= %lu constrained (var %i)\n", bound, boundVar);
                //else LOG(V2_INFO, " -- bound <= %lu not constrained (var %i)\n", bound, boundVar);
            }
            for (auto [weight, lit] : instance.objective) {
                int solLit = (lit>0 ? 1 : -1) * model[std::abs(lit)];
                LOG(V2_INFO, " -- obj. lit %i = %s\n", lit, solLit>0 ? "TRUE" : "FALSE");
            }
            for (int v : outputVars) {
                assert(v > 0);
                assert(model.size() > v);
                int solLit = model[v];
                LOG(V2_INFO, " -- output var %i = %s\n", v, solLit>0 ? "TRUE" : "FALSE");
            }
            LOG(V2_INFO, "full model: %s\n", StringUtils::getSummary(model, INT32_MAX).c_str());
            exit(0);
        }
    }
}

int main() {
    Timer::init();
    Random::init(rand(), rand());
    Logger::init(0, V5_DEBG);
    Process::init(0);
    ProcessWideThreadPool::init(4);
    Parameters params;
#if MALLOB_USE_MAXPRE == 1
    params.maxPre.set(false);
#endif

    FileUtils::mkdir(".tmp");

    //std::vector<int> bounds = {6564,6487,6410,2915,2914,2913,2912,2911,2910,2909,2898,2897,2896,2894,2887,2883,2787,2785,2784,2783,2779,2775,2771,2768,2765,2762,2759,2756,2753,2750,2747,2745,2743,2741,2739,2737,2735,2733,2731,2729,2727,2725,2723,2721,2719,2717,2715,2713,2711,2709,2707,2705,2703,2701,2699,2697,2695,2693,2691,2689,2687,2685,2683,2681,2679,2678,2677,2676,2675,2674,2673,2672,2671,2670,2669,2668,2667,2664,2660,2656,2652,2648,2644,2641,2638,2634,2631,2628,2623,2619,2615,2612,2610,2606,2605,2601,2598,2595,2592,2589,2586,2583,2582,2581,2578,2567,2566,2555,2554,2553,2552,2551,2550,2549,2548,2547,2544,2543,2540,2535,2532,2531,2530,2529,2528,2527,2526,2525,2522,2520,2516,2505,2500,2489,2487,2485,2483,2481,2479,2477,2475,2473,2471,2469,2467,2464,2462,2459,2456,2453,2451,2449,2447,2445,2443,2441,2437,2435,2433,2431,2429,2427,2425,2423,2421,2419,2417,2415,2413,2411,2407,2403,2399,2390,2387,2380,2377,2373,2371,2365,2359,2355,2353,2346,2344,2343,2339,2334,2332};
    //auto instancePath = "instances/wcnf/MaxSATQueriesinInterpretableClassifiers-compas_train_0_DNF_4_1.wcnf";

    //trivialTest();

    //encode(params, "instances/wcnf", "warehouses_wt-warehouse0.wcsp.wcnf");
    //encode(params, "instances/wcnf", "causal-discovery-causal_n5_i4_N500_uai13_harddeps_int.wcnf.xz");
    encode(params, "instances/wcnf", "trivial-pb.wcnf");
}
