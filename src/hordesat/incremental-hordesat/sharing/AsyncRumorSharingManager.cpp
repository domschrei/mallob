/*
 * AsyncRumorSharingManager.cpp
 *
 *  Created on: May 18, 2015
 *      Author: balyo
 */

#include "AsyncRumorSharingManager.h"
#include <algorithm>
#include <math.h>
#include "../utilities/Logger.h"

#define TAG_PULL_REQUEST 10
#define TAG_CLAUSES 20

AsyncRumorSharingManager::AsyncRumorSharingManager(int mpi_size, int mpi_rank, vector<PortfolioSolverInterface*> solvers,
		ParameterProcessor& params):mpi_rank(mpi_rank),mpi_size(mpi_size), recentlyAdded(0),
		solvers(solvers), callback(*this) {
	size_t logProcesses = 1 + (int)log2(mpi_size);
	clManager = new ClauseManager(logProcesses, logProcesses*2, SIGNATURE_SIZE, CLAUSE_BUFFER_SIZE);
	for (size_t i = 0; i < solvers.size(); i++) {
		solvers[i]->setLearnedClauseCallback(&callback, i);
	}
	pushPull = params.isSet("pp");
	if (params.isSet("nls") || solvers.size() <= 1) {
		log(0, "No local clause sharing\n");
		localLearned = NULL;
	} else {
		log(0, "Local clause sharing all-to-all\n");
		localLearned = new vector<vector<int > >[solvers.size()];
	}
	pullRequestPending.reset();
}

bool sortFunction(const vector<int>& i, const vector<int>& j) {
	return (i.size() < j.size());
}

void AsyncRumorSharingManager::doSharing() {
	size_t logProcesses = 1 + (int)log2(mpi_size);

	// local (shared memory) clause exchange
	if (localLearned != NULL && localLearned[0].size() > CLAUSE_PRODUCTION_NORM) {
		localExchangeMutex.lock();
		for (size_t sid = 0; sid < solvers.size(); sid++) {
			for (size_t csid = 0; csid < solvers.size(); csid++) {
				if (csid != sid) {
					solvers[sid]->addClauses(localLearned[csid]);
				}
			}
		}
		for (size_t sid = 0; sid < solvers.size(); sid++) {
			localLearned[sid].clear();
		}
		localExchangeMutex.unlock();
	}

	// adding clauses to the solvers
	if (clausesVector.size() > 2 * CLAUSE_PRODUCTION_NORM * logProcesses) {
		if (recentlyAdded < CLAUSE_PRODUCTION_NORM) {
			for (size_t i = 0; i < solvers.size(); i++) {
				solvers[i]->increaseClauseProduction();
			}
		}
		size_t received = clausesVector.size();
		sort(clausesVector.begin(), clausesVector.end(), sortFunction);
		clausesVector.erase(clausesVector.begin() + CLAUSE_PRODUCTION_NORM * logProcesses, clausesVector.end());
		for (size_t i = 0; i < solvers.size(); i++) {
			solvers[i]->addLearnedClauses(clausesVector);
		}
		stats.importedClauses += clausesVector.size();
		log(1, "Node %d: The core solvers recently produced %lu clauses, each will import %lu clauses, received %lu\n",
				mpi_rank, recentlyAdded, clausesVector.size(), received);
		stats.sharedClauses += recentlyAdded;
		recentlyAdded = 0;
		clausesVector.clear();
	}

	// Check if sends are finished and return buffers
	for (list<AsyncRequest>::iterator it = unfinishedRequests.begin(); it != unfinishedRequests.end();) {
		int flag = 0;
		MPI_Test(&it->request, &flag, MPI_STATUS_IGNORE);
		if (flag) {
			bufferManager.returnBuffer(it->buffer);
			it = unfinishedRequests.erase(it);
		} else {
			it++;
		}
	}

	int flag = 0;
	MPI_Status status;

	// Check for incomming pull requests
	MPI_Iprobe(MPI_ANY_SOURCE, TAG_PULL_REQUEST, MPI_COMM_WORLD, &flag, &status);
	while (flag) {
		// Received a pull request
		int* sigBuff = bufferManager.getBuffer(SIGNATURE_SIZE);
		// receive the signature
		MPI_Recv(sigBuff, SIGNATURE_SIZE, MPI_INT, status.MPI_SOURCE, TAG_PULL_REQUEST, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
		AsyncRequest arq;
		arq.buffer = bufferManager.getBuffer(CLAUSE_BUFFER_SIZE);
		clManager->filterHot(arq.buffer, sigBuff);
		MPI_Isend(arq.buffer, CLAUSE_BUFFER_SIZE, MPI_INT, status.MPI_SOURCE, TAG_CLAUSES, MPI_COMM_WORLD, &arq.request);
		log(2, "Node %d sent %d clauses to %d\n", mpi_rank, arq.buffer[0], status.MPI_SOURCE);

		// push-pull
		if (pushPull) {
			if (!pullRequestPending.test(status.MPI_SOURCE) &&
					clManager->getSignatureDiffCount(sigBuff) > CLAUSE_PRODUCTION_NORM) {
				log(2, "%d sent PUSH-PULL request to %d\n", mpi_rank, status.MPI_SOURCE);
				sendPullRequest(status.MPI_SOURCE);
			}
		}

		clManager->nextRound();
		unfinishedRequests.push_back(arq);
		bufferManager.returnBuffer(sigBuff);

		flag = 0;
		MPI_Iprobe(MPI_ANY_SOURCE, TAG_PULL_REQUEST, MPI_COMM_WORLD, &flag, &status);
	}

	// Check for incomming clauses
	flag = 0;
	MPI_Iprobe(MPI_ANY_SOURCE, TAG_CLAUSES, MPI_COMM_WORLD, &flag, &status);
	vector<vector<int> > vipClauses;
	while (flag) {
		// Received clauses
		int* clsBuffer = bufferManager.getBuffer(CLAUSE_BUFFER_SIZE);
		MPI_Recv(clsBuffer, CLAUSE_BUFFER_SIZE, MPI_INT, status.MPI_SOURCE, TAG_CLAUSES, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
		pullRequestPending.reset(status.MPI_SOURCE);
		int imported = clManager->importClauses(clsBuffer, clausesVector, vipClauses);
		bufferManager.returnBuffer(clsBuffer);
		log(2, "%d received %d clauses from %d and imported %d\n", mpi_rank, clsBuffer[0], status.MPI_SOURCE, imported);

		flag = 0;
		MPI_Iprobe(MPI_ANY_SOURCE, TAG_CLAUSES, MPI_COMM_WORLD, &flag, &status);
	}
	if (!vipClauses.empty()) {
		log(2, "adding %lu vip clauses\n", vipClauses.size());
		for (size_t i = 0; i < solvers.size(); i++) {
			solvers[i]->addLearnedClauses(vipClauses);
		}
	}

	// Send a random pull request
	if (unfinishedRequests.size() < logProcesses) {
		int dest = rand() % mpi_size;
		if (dest == mpi_rank) {
			dest = (dest + 1) % mpi_size;
		}
		if (!pullRequestPending.test(dest)) {
			log(2, "%d sent pull request to %d\n", mpi_rank, dest);
			sendPullRequest(dest);
		}
	}

	/*
	Hallo Tomas,

	I have thought some more about asynchronous protocols for clause
	exchange. I have another proposal now that has some advantages:
	- higher algorithmic "coolness"
	- less tuning parameters
	- hopefully lower communication volume
	- all PEs will eventually get important new clauses

	Rather than the "push"-protocoll we have discussed, we should use a
	"pull"-protocol:
	Its also less important to get the period length right since eventually,
	less
	Each PE periodically sends a succinct approximate repesentation C of its
	clause set (similar to the Bloom Filter you use right now but one shoult
	probably use sth more efficient) to a random PE b. PE b sends those of
	its clauses that are not in C back to a. Optionally, one could make that
	push-pull -- b requests back those clauses of a that it does not have.

	We save the tuning parameters about the outdegree of rumour spreading.
	The interval where our Bloom filters are cleared we had anyway.
	We may still need a grace period where we do not send new requests.
	Anyway, a request should not be sent
	- as long  as no reply has been received for the last one
	- as long as there are unserved incoming requests.

	A fine point is that at least in theory, we should use a fresh hash
	function for the Bloom filter every time (or at least often). Then
	we can prove that every PE will eventually get all the clauses. The
	downside is that we have to rebuild it often. But we can make that more
	efficient:
	- use uncompressed representation locally
	- use only a single hash function and a compressed
	   single shot Bloom filter with a fast compression algorithm

	Best regards

	Details about hash function:
	H(clause) -> 64 bit integer x,
	universal hashing on x into a k-bit number, store to a 2^k sized bitmap B (single shot Bloom Filter)
	choose k such that the avg difference between ones in B is ~ 128-256
	compress B and send it along with hash function used from the universal family
	to compress B encode the distances between ones as 8 bit numbers, 0x0000 means 256
	B is kept as a sorted list of non-zero positions (easy to add and merge)
	*/
}

void AsyncRumorSharingManager::sendPullRequest(int destination) {
	AsyncRequest arq;
	arq.buffer = bufferManager.getBuffer(SIGNATURE_SIZE);
	clManager->getSignature(arq.buffer);
	MPI_Isend(arq.buffer, SIGNATURE_SIZE, MPI_INT, destination, TAG_PULL_REQUEST, MPI_COMM_WORLD, &arq.request);
	unfinishedRequests.push_back(arq);
	pullRequestPending.set(destination);
}


SharingStatistics AsyncRumorSharingManager::getStatistics() {
	return stats;
}

AsyncRumorSharingManager::~AsyncRumorSharingManager() {
}

