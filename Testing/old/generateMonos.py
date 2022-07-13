s = "#!/bin/bash\n"
maxNp = 8
kList = [5,10,20,30,40]
npList = [64, 32, 16, 8, 4, 2, 1] # range(maxNp,0,-1)
for k in kList:
    for n in npList:    
        s += "echo 'k="+str(k)+" n="+str(n)+"' >> ./Testing/Out"+str(npList[0])+"."+str(k)+".txt ;PATH=build/:$PATH RDMAV_FORKSAVE=1 mpirun -np "+ str(n) +" build/mallob -mono-application=KMEANS -mono=./instances/covtypeShuffle"+ str(k) +".csv -v=0 >> ./Testing/Out"+str(npList[0])+"."+str(k)+".txt;"
import clipboard as c
c.copy(s[:len(s)-1])
print(s[:len(s)-1])