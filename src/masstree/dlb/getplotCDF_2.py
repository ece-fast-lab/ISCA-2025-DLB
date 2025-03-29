import numpy as np
import matplotlib.pyplot as plt
from statistics import mean

data11 = np.loadtxt('get.txt')

data_size11=len(data11)

# Set bins edges
data_set11=sorted(set(data11))
bins11=np.append(data_set11, data_set11[-1]+1)

# Use the histogram function to bin the data
counts11, bin_edges11 = np.histogram(data11, bins=bins11, density=False)

counts11=counts11.astype(float)/data_size11

# Find the cdf
cdf11 = np.cumsum(counts11)

# Plot the cdf
plt.plot(bin_edges11[0:-1], cdf11,linestyle='--', color='b')
plt.axvline(mean(data11), color='b')

print(mean(data11))
"""
data12 = np.loadtxt('get_new_2.txt')

data_size12=len(data12)

# Set bins edges
data_set12=sorted(set(data12))
bins12=np.append(data_set12, data_set12[-1]+1)

# Use the histogram function to bin the data
counts12, bin_edges12 = np.histogram(data12, bins=bins12, density=False)

counts12=counts12.astype(float)/data_size12

# Find the cdf
cdf12 = np.cumsum(counts12)

# Plot the cdf
plt.plot(bin_edges12[0:-1], cdf12,linestyle='--', color='g')



data13 = np.loadtxt('get_new_3.txt')

data_size13=len(data13)

# Set bins edges
data_set13=sorted(set(data13))
bins13=np.append(data_set13, data_set13[-1]+1)

# Use the histogram function to bin the data
counts13, bin_edges13 = np.histogram(data13, bins=bins13, density=False)

counts13=counts13.astype(float)/data_size13

# Find the cdf
cdf13 = np.cumsum(counts13)

# Plot the cdf
plt.plot(bin_edges13[0:-1], cdf13,linestyle='--', color='r')
"""


plt.ylim((0,1))
plt.xlim((0,10000)) #use 20 for gets
plt.ylabel("CDF")
plt.xlabel("Service Time (ns)")
#plt.legend(["Query Length (64)", "Query Length (32)","Query Length (16)","Query Length (8)","Query Length (4)","Query Length (2)","Query Length (1)"], loc ="lower right")
plt.title("Get Query Service Time Distribution")
plt.grid(True)


plt.show()
