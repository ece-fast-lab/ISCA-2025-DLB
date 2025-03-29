import numpy as np
import matplotlib.pyplot as plt

data = np.loadtxt('lat.txt')

data_size=len(data)

# Set bins edges
data_set=sorted(set(data))
bins=np.append(data_set, data_set[-1]+1)

# Use the histogram function to bin the data
counts, bin_edges = np.histogram(data, bins=bins, density=False)

counts=counts.astype(float)/data_size

# Find the cdf
cdf = np.cumsum(counts)

# Plot the cdf
plt.plot(bin_edges[0:-1], cdf,linestyle='--', marker="o", color='b')
plt.ylim((0,1))
plt.xlim((0,2000)) #use 20 for gets
plt.ylabel("CDF")
plt.grid(True)

plt.show()
