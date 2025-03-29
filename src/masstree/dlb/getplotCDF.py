import numpy as np
import matplotlib.pyplot as plt

data11 = np.loadtxt('get_rw1_1.txt')

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


data12 = np.loadtxt('get_rw1_2.txt')

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



plt.ylim((0,1))
plt.xlim((0,10000)) #use 20 for gets
plt.ylabel("CDF")
plt.xlabel("Service Time (ns)")
#plt.legend(["Query Length (64)", "Query Length (32)","Query Length (16)","Query Length (8)","Query Length (4)","Query Length (2)","Query Length (1)"], loc ="lower right")
plt.title("Get Query Service Time Distribution")
plt.grid(True)


plt.show()
