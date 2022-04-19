#include "kmeans_utils.hpp"
#include <vector>
namespace KMeansUtils {
    KMeansInstance loadPoints(JobDescription& desc) {
        const int* payload = desc.getFormulaPayload(0);
        KMeansInstance result;
        result.numClusters = payload[0]; 
        result.dimension = payload[1];  
        result.pointsCount = payload[2];
        result.data.reserve(result.pointsCount);
        payload += 3; //pointer start at first datapoint instead of metadata
        for (int point = 0; point < result.pointsCount; ++point) {
            Point p;
            p.reserve(result.dimension);
            for (int entry = 0; entry < result.dimension; ++entry) {
                p.push_back(*((float*) (payload + entry)));
            }
            result.data.push_back(p);
            payload = payload + result.dimension;
        }
        return result;
    }

    ClusterMembership calcNearestCenter(KMeansData dataPoints, ClusterCenters clusters) {

    }
    ClusterCenters calcCurrentClusterCenters(KMeansData dataPoints, ClusterMembership clusters){
        
    }
}