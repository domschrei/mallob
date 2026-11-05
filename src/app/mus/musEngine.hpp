#ifndef MALLOB_MUSENGINE_HPP
#define MALLOB_MUSENGINE_HPP

#include "app/sat/execution/scatterengine.hpp"

class MusEngine : public ScatterEngine {

public:
	MusEngine();

    static int getNumSolversForNumCores(int numCores) { return 2 * numCores; }

};

#endif //MALLOB_MUSENGINE_HPP
