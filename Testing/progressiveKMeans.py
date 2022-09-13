import time
import warnings

import numpy as np
import matplotlib.pyplot as plt

from sklearn import cluster, datasets, mixture
from sklearn.neighbors import kneighbors_graph
from sklearn.preprocessing import StandardScaler
from itertools import cycle, islice

np.random.seed(2)


# blobs with varied variances
n_samples = 200
random_state = 40
#blobs = datasets.make_blobs(n_samples=n_samples, random_state=8)
varied = datasets.make_blobs(
    n_samples=n_samples, cluster_std=[1.0, 2.5, 0.7], random_state=random_state
)

# ============
# Set up cluster parameters
# ============
plt.figure(figsize=(9 * 2 + 3, 18))
plt.subplots_adjust(
    left=0.02, right=0.98, bottom=0.001, top=0.95, wspace=0.05, hspace=0.01
)

plot_num = 1

default_base = {
    "quantile": 0.3,
    "max_iter":1,
    "eps": 0.3,
    "damping": 0.9,
    "preference": -200,
    "n_neighbors": 3,
    "n_clusters": 3,
    "min_samples": 7,
    "xi": 0.05,
    "min_cluster_size": 0.1,
}

datasets = [
    (
        varied,
        {
            "eps": 0.18,
            "n_neighbors": 2,
            "min_samples": 7,
            "xi": 0.01,
            "min_cluster_size": 0.2,
        },
    ),
]

for i_dataset, (dataset, algo_params) in enumerate(datasets):
    # update parameters with dataset-specific values
    params = default_base.copy()
    params.update(algo_params)

    X, y = dataset

    # normalize dataset for easier parameter selection
    X = StandardScaler().fit_transform(X)

    # estimate bandwidth for mean shift
    bandwidth = cluster.estimate_bandwidth(X, quantile=params["quantile"])

    # connectivity matrix for structured Ward
    connectivity = kneighbors_graph(
        X, n_neighbors=params["n_neighbors"], include_self=False
    )
    # make connectivity symmetric
    connectivity = 0.5 * (connectivity + connectivity.T)

    # ============
    # Create cluster objects
    # ============
    iterations = 4
    centroids = None
    two_means = cluster.KMeans(n_clusters=params["n_clusters"], max_iter=1,n_init=1, init=("random"))

    clustering_algorithms = (
        ("", two_means),
        ("", two_means),
        ("", two_means),
        ("", two_means),
        #("Affinity\nPropagation", affinity_propagation),
        #("MeanShift", ms),
        #("Spectral\nClustering", spectral),
        #("Ward", ward),
        #("Agglomerative\nClustering", average_linkage),
        #("DBSCAN", dbscan),
        #("OPTICS", optics),
        #("BIRCH", birch),
        #("Gaussian Mixture", gmm),
    )

    i=1
    maxIter=1
    for name, algorithm in clustering_algorithms:
        t0 = time.time()
        algorithm = cluster.KMeans(n_clusters=params["n_clusters"], max_iter=maxIter,n_init=1, init=(centroids if centroids is not None else 'random'))

        # catch warnings related to kneighbors_graph
        with warnings.catch_warnings():
            warnings.filterwarnings(
                "ignore",
                message="the number of connected components of the "
                + "connectivity matrix is [0-9]{1,2}"
                + " > 1. Completing it to avoid stopping the tree early.",
                category=UserWarning,
            )
            warnings.filterwarnings(
                "ignore",
                message="Graph is not fully connected, spectral embedding"
                + " may not work as expected.",
                category=UserWarning,
            )
            result = algorithm.fit(X)
            centroids = algorithm.cluster_centers_
            print(result.n_iter_)

        t1 = time.time()
        if hasattr(algorithm, "labels_"):
            y_pred = algorithm.labels_.astype(int)
        else:
            y_pred = algorithm.predict(X)

        plt.subplot(int(len(clustering_algorithms)/2), int(len(clustering_algorithms)/2), plot_num)
        if i_dataset == 0:
            plt.title(name, size=30)

        colors = np.array(
            list(
                islice(
                    cycle(
                        [
                            "#377eb8",
                            "#ff7f00",
                            "#4daf4a",
                            "#f781bf",
                            "#a65628",
                            "#984ea3",
                            "#999999",
                            "#e41a1c",
                            "#dede00",
                        ]
                    ),
                    int(max(y_pred) + 1),
                )
            )
        )
        markers = np.array(
            list(
                islice(
                    cycle(
                        [
                            "^",
                            "o",
                            "s",
                        ]
                    ),
                    int(max(y_pred) + 1),
                )
            )
        )
        # add black color for outliers (if any)
        colors = np.append(colors, ["#000000"])
        plt.scatter(X[:, 0][y_pred==0], X[:, 1][y_pred==0], s=300, color="#377eb8", marker="^")
        plt.scatter(X[:, 0][y_pred==1], X[:, 1][y_pred==1], s=300, color="#ff7f00", marker="s")
        plt.scatter(X[:, 0][y_pred==2], X[:, 1][y_pred==2], s=300, color="#4daf4a", marker="o")

        
        plt.scatter(centroids[0, 0], centroids[0, 1], s=500, color="#000000", marker="^")
        plt.scatter(centroids[1, 0], centroids[1, 1], s=500, color="#000000", marker="s")
        plt.scatter(centroids[2, 0], centroids[2, 1], s=500, color="#000000", marker="o")

        plt.xlim(-2.5, 2.5)
        plt.ylim(-2.5, 2.5)
        plt.xticks(())
        plt.yticks(())
        plt.text(
            0.99,
            0.01,
            "iteration: "+ str(maxIter*(plot_num-1)),
            transform=plt.gca().transAxes,
            size=50,
            horizontalalignment="right",
        )
        plot_num += 1

plt.savefig("KMeansExample.pdf", dpi=300)