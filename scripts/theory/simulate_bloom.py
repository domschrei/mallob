
import math
import random
import matplotlib.pyplot as plt

primes = [2038072819, 2038073287, 2038073761, 2038074317, 2038072823, 2038073321, 2038073767, 2038074319, 2038072847, 2038073341, 2038073789, 2038074329, 2038074751, 2038075231, 2038075751, 2038076267]

def hash_cls(cls, which):
    res = 0
    for l in cls:
        res = res ^ (l * primes[(l^which) & 15])
    return res

def random_cls():
    size = round(1)#+random.random()*9)
    cls = []
    for i in range(size):
        l = 1+int(random.random()*200000)
        if random.random() < 0.5:
            l = -l
        cls += [l]
    return tuple(cls)


"""
X = []
Y = []

for i in range(1000, 100001000, 1000):
    X += [i]
    Y += [i * (1 - math.e**(-4*i/26843543))**4]

plt.plot(X, Y)
plt.show()

exit(0)

print(1000,(1 - math.e**(-4*1000/26843543))**4)
print(10000,(1 - math.e**(-4*10000/26843543))**4)
print(100000,(1 - math.e**(-4*100000/26843543))**4)
print(500000,(1 - math.e**(-4*500000/26843543))**4)
print(1000000,(1 - math.e**(-4*1000000/26843543))**4)
print(10000000,(1 - math.e**(-4*10000000/26843543))**4)
#exit(0)
"""

for t in range(1):

    memsize = 26843543
    bloom = [False for x in range(memsize)]
    num_funcs = 4

    def register(cls):
        contained = True
        for i in range(num_funcs):
            idx = hash_cls(cls,i)%memsize
            contained = contained and bloom[idx]
            bloom[idx] = True
        return not contained



    set_of_cls = set()

    for n in range(1000000):
        cls = random_cls()
        if not register(cls):
            if cls not in set_of_cls:
                print("Collision")
        set_of_cls.add(cls)
