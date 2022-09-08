


def main():
    import time
    import numpy as np
    from sklearn.datasets import make_blobs
    
    np.set_printoptions(suppress=True)

       
    floats = []
    with open("/mnt/c/Projekte/mallob/instances/covtypeShuffleK.csv") as f:
        for line in f:
            line = line.strip()
            for number in line.split():
                floats.append(float(number))

    
    k, dimension, skipCols, pointsCount = 50, 54, 55, 581012
    X = np.ndarray([pointsCount,dimension])
    for point in range(pointsCount):
        for entry in range(dimension):
            X[point,entry] = floats[point*(skipCols)+entry]
            #if point == 2: print(str(X[point][entry]) + " ")
            
        
    


    from sklearn.cluster import KMeans
    startPoints = []

    for i in range(k):
        print(int((float(i) / float(k)) * (pointsCount - 1)))
        startPoints.append(X[int((float(i) / float(k)) * (pointsCount - 1))])
    for i in range(k):
        print(startPoints[i])
    
    
    print("\n")
    

    km = KMeans(
        n_clusters=k, init=startPoints,
        n_init=1, max_iter=3000, 
        tol=0, random_state=0, algorithm="lloyd"
    )

    print("start")
    start_time = time.time() 
    y_km = km.fit(X)
    end_time = time.time()

    for i in range(k):
        print(y_km.cluster_centers_[i])
    print()
    print(y_km.n_iter_)
    print(end_time - start_time)

main()