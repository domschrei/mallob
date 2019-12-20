import sys
import matplotlib
import matplotlib.pyplot as plt
from matplotlib.markers import MarkerStyle
import numpy as np
import re
import math
import sys
from matplotlib.ticker import MaxNLocator

# The domain of the problem which is being plotted.
# It must be the same for all read data points.
domain = None

timeout_seconds = 300.0

"""
Reads a data file.
@param datafile (string) path to the file
"""
def read_datafile(datafile):

    global domain

    # Structure for values 
    # Key: competitor label, Value: list of index-time pairs
    values = []

    # Read data file line by line
    f = open(datafile, 'r')
    for l in f.readlines():
        
        # Clean line and separate by whitespace
        l = l.replace(" \n", "")
        l = l.replace("\n", "")
        words = l.split(" ")
        
        values += [float(words[0])]
        
    return values

def plot(competitor_values, competitor_labels, colors_of_competitors, directory=".", output_file=None, logscale=False, heading="", ylabel=""):
    
    ax = plt.figure(figsize=(5, 3.5)).gca()
    colors = ['#377eb8', '#ff7f00', '#e41a1c', '#f781bf', '#a65628', '#4daf4a', '#999999', '#984ea3', '#dede00']
    markers = ['s', '^', 'o', '*', '+', 'x']
    
    # For each competitor
    for i in range(len(competitor_values)):
        
        values = competitor_values[i]
        label = competitor_labels[i]
        
        X = []
        Y = []
        solved_instances = 0
        
        for v in values:
            solved_instances += 1
            X += [solved_instances]
            Y += [v]
        
        # Color (if defined)
        c = None
        if label in colors_of_competitors:
            c = colors_of_competitors[label]
            print(c)
        
        # Plot
        if c is not None:
            plt.plot(X, Y, color=c, label=label)
        else:
            plt.plot(X, Y, marker=markers[i % len(markers)], mec=colors[i % len(colors)], mfc='#ffffff00', color=colors[i % len(colors)], linestyle='--', linewidth=1, label=label)
    
    # Some displaying options and labelling
    ax.xaxis.set_major_locator(MaxNLocator(integer=True))
    if logscale:
        ax.set_yscale('log') # Logscale in y direction
    #ax.set_aspect(0.18)
    plt.legend()
    if heading:
        plt.title(heading)
    plt.ylabel(ylabel)
    plt.xlim(0)
    plt.ylim(0)
    #plt.ylim(0, timeout_seconds)
    plt.xlabel("Number of solved instances")
    
    plt.tight_layout()
    
    # Show or save plot
    if output_file is None:
        plt.show()
    else:
        plt.savefig(output_file)


def print_usage():
    print("Usage : " + sys.argv[0] + " <Datafile> <Datafile options> [<Datafile> <Datafile options> ...] [<Color options>] [<General options>]")
    print()
    print("General options:")
    print("  --logscale    Displays the plot's y values in logarithmic scale.")
    print("  -t<seconds>   Assume a timeout value of the provided amount of seconds for this visualization")
    print("  -o<filename>  Outputs the plot to the specified file instead of displaying it in a window")
    print("  -h<heading>   Provides a caption as the plot's heading")
    print("  -y<label>     Label of the y axix")
    print()
    print("Datafile options:")
    print("  -l<label>")
    print("      Defines the competitor label for the respective datafile")
    print()
    print("Color options:")
    print("  -c<color>")
    print("      Renders the competitor corresponding to the provided label")
    print("      (already renamed, if applicable) in the specified color")

def main():

    global timeout_seconds

    # Data read from the arguments
    datafiles = []
    label_replacements = []
    colors_of_competitors = dict()
    colors = []
    output_file_plot = None
    logscale = False
    heading = ""
    ylabel = "Time limit / s"

    # Print usage, if not enough arguments
    if len(sys.argv) <= 1:
        print_usage()
        exit()
    
    competitor_labels = []
    
    # Parse arguments
    for arg in sys.argv[1:]:
        
        # Logscale option
        if arg == '--logscale':
            logscale = True
        
        # Competitor label replacement option
        elif arg[0:2] == '-l':
            match = re.search(r'-l(.+)', arg)
            if match is not None:            
                competitor_labels[-1] = match.group(1)
        
        # Color option for a competitor
        elif arg[0:2] == '-c':
            match = re.search(r'-c(.+):(.+)', arg)
            if match is not None:
                colors_of_competitors[match.group(1)] = match.group(2)
        
        # Plot output file
        elif arg[0:2] == '-o':
            match = re.search(r'-o(.+)', arg)
            if match is not None:
                output_file_plot = match.group(1)
    
        # Timeout value
        elif arg[0:2] == '-t':
            match = re.search(r'-t([0-9]+)', arg)
            if match is not None:
                timeout_seconds = int(match.group(1))
        
        elif arg[0:2] == '-h':
            match = re.search(r'-h(.+)', arg)
            if match is not None:
                heading = match.group(1)
        
        elif arg[0:2] == '-y':
            match = re.search(r'-y(.+)', arg)
            if match is not None:
                ylabel = match.group(1)
        
        # Datafile specification
        else:
            datafiles += [arg]
            label_replacements += [[]]
            colors += [[]]
            competitor_labels += ["unnamed " + str(len(competitor_labels))]

    # Structures to save the data to plot
    competitor_values = []
    
    # For each specified datafile
    for i in range(len(datafiles)):

        # Identify data file
        datafile = datafiles[i]
        
        # Read data file
        new_values = read_datafile(datafile)
        
        # Replace labels, if necessary
        for [str_old, str_new] in label_replacements[i]:
            new_values[str_new] = new_values.pop(str_old)
        
        # Update total values dictionary
        competitor_values += [new_values]


    # Plot and (display or save)
    plot(competitor_values, competitor_labels, colors_of_competitors, output_file=output_file_plot, logscale=logscale, heading=heading, ylabel=ylabel)
    
if __name__ == '__main__':
    main()
