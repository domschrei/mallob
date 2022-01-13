
m = 2
j = 80

while m <= 128:
    
    print(m)
    d = "RESULTS/mallob_mono_cbdf0.875_mlbd00_mcl0_n" + str(m)
    
    runtimes = []
    for line in open(d + "/cdf_runtimes", 'r').readlines():
        line = line.rstrip()
        words = line.split(" ")
        time = float(words[0])
        runtimes += [time]
        
    outfile = open(d + "/optimal_scheduler_times", "w")
    
    nslots = int(128 / m)
    endtimes = [0] * nslots
    solved = 0
    last_runtime = 0
    while runtimes or solved < j:
        
        # Select next slot (with lowest endtime)
        min_endtime = 999999
        min_index = -1
        for i in range(len(endtimes)):
            if endtimes[i] < min_endtime:
                min_endtime = endtimes[i]
                min_index = i
        
        # Schedule next job
        solved += 1
        if runtimes:
            rt = runtimes[0]
            del runtimes[0]
            endtimes[min_index] += rt
            print("Runtime:",rt)
            print("Select slot", min_index,": done @",endtimes[min_index])
            outfile.write("%.3f %i\n" % (endtimes[min_index], solved))
        else:
            endtimes[min_index] += rt
            print("*Runtime:",rt)
            print("*Select slot", min_index,": done @",endtimes[min_index])
            if solved == j:
                # End: set last "virtual" point
                outfile.write("%.3f %i\n" % (endtimes[min_index], solved))
    
    m *= 4
