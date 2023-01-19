
#include "historic_clause_storage.hpp"

bool areIntervalsOverlapping(int begin1, int end1, int begin2, int end2) {
    return (begin1 >= begin2 && begin1 < end2) || (end1 > begin2 && end1 <= end2);
}
