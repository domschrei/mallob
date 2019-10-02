/*
 * AsyncRumorSharingManager.h
 *
 *  Created on: May 18, 2015
 *      Author: balyo
 */

#ifndef SHARING_ASYNCRUMORSHARINGMANAGER_H_
#define SHARING_ASYNCRUMORSHARINGMANAGER_H_

#include "../utilities/mympi.h"
#include "AllToAllSharingManager.h"
#include "../utilities/ClauseManager.h"
#include "../utilities/BufferManager.h"
#include "../utilities/Threading.h"
#include <list>
#include <bitset>

// number of ints in the signature
#define SIGNATURE_SIZE 32768
// number of ints in the clause buffer
#define CLAUSE_BUFFER_SIZE 4096
// number of clauses that each core solver should produce per second
#define CLAUSE_PRODUCTION_NORM 100


struct AsyncRequest {
	MPI_Request request;
	int* buffer;
};

class AsyncRumorSharingManager : public SharingManagerInterface {

private:
	int mpi_rank, mpi_size;
	size_t recentlyAdded;
	bool pushPull;
	SharingStatistics stats;
	list<AsyncRequest> unfinishedRequests;
	ClauseManager* clManager;
	BufferManager bufferManager;
	vector<vector<int> > clausesVector;
	vector<PortfolioSolverInterface*> solvers;
	vector<vector<int> >* localLearned;
	//FIXME should not be constant
	bitset<32768> pullRequestPending;
	Mutex localExchangeMutex;

	class Callback : public LearnedClauseCallback {
	public:
		AsyncRumorSharingManager& parent;
		Callback(AsyncRumorSharingManager& parent):parent(parent) {
		}
		void processClause(vector<int>& cls, int solverId) {
			if (parent.clManager->addClause(cls) == false) {
				parent.stats.dropped++;
			} else {
				if (parent.localLearned != NULL) {
					if (parent.localExchangeMutex.tryLock()) {
						parent.localLearned[solverId].push_back(cls);
						parent.localExchangeMutex.unlock();
					}
				}
				parent.recentlyAdded++;
			}
		}
	};

	Callback callback;

	void sendPullRequest(int destination);


public:
	AsyncRumorSharingManager(int mpi_size, int mpi_rank, vector<PortfolioSolverInterface*> solvers,
			ParameterProcessor& params);
	void doSharing();
    inline std::vector<int> prepareSharing(int size) {return std::vector<int>();};
    inline void digestSharing() {};
    inline void digestSharing(const std::vector<int>& result) {};
	SharingStatistics getStatistics();
	virtual ~AsyncRumorSharingManager();
};

#endif /* SHARING_ASYNCRUMORSHARINGMANAGER_H_ */
