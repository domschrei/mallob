
#ifndef MALLOB_KISSAT_SWEEP_FUNCS_HPP
#define MALLOB_KISSAT_SWEEP_FUNCS_HPP

#include "app/sat/solvers/kissat.hpp"

enum class SweepStealType {
	MPI   = 1,
	Local = 2,
};

//Tracking worksteal events in MallobSweep
struct SweepStealInfo {
	int		nr;
	SweepStealType stealtype;
	int		size;
	float	t_submit;
	float	t_receive;
	float   t_read;
	int		round;
};


class SweepJob; //fwd decl

class KissatSweep : public Kissat {
private:
	int representative_localId = 0;
	friend class SweepJob; //fwd decl

	//transfer a single equivalence from C to C++
	std::vector<int> eq_up_buffer = std::vector<int>(2);

	//accumulate unit and equivalences for sharing
	//when exporting data from the solver to Mallob, need to lock the vector when extracting for global sharing,
	//otherwise the solver threads might concurrently push new data into the std::vector while we are reading it
	std::vector<int> units_to_share;
	std::vector<int> eqs_to_share;
	std::mutex sweep_export_mutex;

	//Work received by stealing from others
	//This vector is allocated and controlled on the C++ level, but accessible and operated on also within the kissat solver
	//on the C level. The kissat solver thus trusts that this array is always available and never suddenly reallocated.
	std::vector<int> work_received_from_steal;

	//Lock that makes sure that only one other solver can steal from this solver at a time
	std::atomic_flag steal_victim_lock = ATOMIC_FLAG_INIT;

	//Tracking whether this solver is idle (searching for work)
	std::atomic_bool sweeper_is_idle = false;
	std::atomic_bool sweeper_longterm_idle = false;

	//The solver tracks which data it already imported that arrived via a global sharing round
	int curr_eq_round{0};
	int curr_eq_index{0};
	int curr_unit_round{0};
	int curr_unit_index{0};

	//Some more detailed information collection on stealing, for debugging
	int attempted_steals = 0;
	std::vector<SweepStealInfo> steal_records{};
	struct shweep_statistics sweep_stats{};
	
	//Shared Sweeping / SWEEP App
	friend void sweep_export_eq(void *state);
	friend void sweep_export_unit(void *state, int unit);
	void triggerSweepTerminate();
	void setRepresentativeLocalId(int localId);
	bool set_option(const std::string &option_name, int value);

	shweep_statistics fetchSweepStats();
	void sweepExportEq();
	void sweepExportUnit(int eunit);
	void sweepSetExportCallbacks();
	
public:
	KissatSweep(const SolverSetup &setup);
	 ~KissatSweep() override = default;
};


inline KissatSweep::KissatSweep(const SolverSetup& setup) : Kissat(setup) {}

inline void sweep_export_eq(void *state) {
    ((KissatSweep*) state)->sweepExportEq();
}

inline void sweep_export_unit(void *state, int unit) {
    ((KissatSweep*) state)->sweepExportUnit(unit);
}

//Tighter handling on option setting, triggers an assertion if an option can't be applied
inline bool KissatSweep::set_option(const std::string &option_name, int value) {
    int prev_value = kissat_get_option(solver, option_name.c_str());
    kissat_set_option(solver, option_name.c_str(), value);
    int set_value = kissat_get_option(solver, option_name.c_str());

    if (set_value != value) {
        LOGGER(_logger, V0_CRIT, "ERROR Setting Kissat Option %s: %i --> %i failed, remained at %i (or option not found)\n", option_name.c_str(), prev_value, value, prev_value);
        assert(false);
        return false;
    }
    // LOGGER(_logger, V3_VERB, "Kissat Set Option: %i --> %i (%s)\n", prev_value, value, option_name.c_str());
    return true;
}

inline void KissatSweep::triggerSweepTerminate() {
    shweep_set_end_job_signal(solver);
}

inline void KissatSweep::sweepSetExportCallbacks() {
    shweep_set_equivalence_export_callback(solver, this, eq_up_buffer.data(), &sweep_export_eq);
    shweep_set_unit_export_callback(solver, this, &sweep_export_unit);
}

inline void KissatSweep::sweepExportEq() {
    {
        std::lock_guard<std::mutex> lock(sweep_export_mutex); //dont push something when the aggregation thread is just touching the eqs_to_share vector
        const int elit1 = eq_up_buffer[0];
        const int elit2 = eq_up_buffer[1];
        eqs_to_share.push_back(elit1);
        eqs_to_share.push_back(elit2);
        assert((elit1 !=0 &&  elit2!=0) || log_return_false("SWEEP ERROR: in exportEq: elit is zero. elit1=%i, elit2=%i. (buffersize: %i)\n", elit1, elit2, eq_up_buffer.size()));
        assert(std::abs(elit1) < std::abs(elit2) || log_return_false("SWEEP ERROR: in exportEq: abs(elit1) is larger than abs(elit2), but it should be smaller. elit1=%i, elit2=%i. (buffersize: %i)\n", elit1, elit2, eq_up_buffer.size()));
        // LOG(V1_WARN, "(%i) exported e[%i,%i]\n", getLocalId(), elit1, elit2);
    }
}

inline void KissatSweep::sweepExportUnit(int eunit) {
    {
        std::lock_guard<std::mutex> lock(sweep_export_mutex);
        assert(eunit!=0 || log_return_false("SWEEP ERROR: in exportUnit: eunit is zero.\n"));
        units_to_share.push_back(eunit);
    }
}

inline shweep_statistics KissatSweep::fetchSweepStats() {
    sweep_stats = shweep_get_statistics(solver);
    return sweep_stats;
}

inline void KissatSweep::setRepresentativeLocalId(int localId) {
    representative_localId = localId;
}

#endif //MALLOB_KISSAT_SWEEP_FUNCS_HPP
