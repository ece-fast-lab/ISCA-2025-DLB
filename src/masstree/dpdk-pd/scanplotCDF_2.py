import numpy as np
import matplotlib.pyplot as plt
from statistics import mean

data256 = np.loadtxt('scan_256.txt')

data_size256=len(data256)

# Set bins edges
data_set256=sorted(set(data256))
bins256=np.append(data_set256, data_set256[-1]+1)

# Use the histogram function to bin the data
counts256, bin_edges256 = np.histogram(data256, bins=bins256, density=False)

counts256=counts256.astype(float)/data_size256

# Find the cdf
cdf256 = np.cumsum(counts256)

# Plot the cdf
plt.plot(bin_edges256[0:-1], cdf256,linestyle='--', color='r')

print(mean(data256))



data128 = np.loadtxt('scan_128.txt')

data_size128=len(data128)

# Set bins edges
data_set128=sorted(set(data128))
bins128=np.append(data_set128, data_set128[-1]+1)

# Use the histogram function to bin the data
counts128, bin_edges128 = np.histogram(data128, bins=bins128, density=False)

counts128=counts128.astype(float)/data_size128

# Find the cdf
cdf128 = np.cumsum(counts128)

# Plot the cdf
plt.plot(bin_edges128[0:-1], cdf128,linestyle='--', color='b')

print(mean(data128))

data64 = np.loadtxt('scan_64.txt')

data_size64=len(data64)

# Set bins edges
data_set64=sorted(set(data64))
bins64=np.append(data_set64, data_set64[-1]+1)

# Use the histogram function to bin the data
counts64, bin_edges64 = np.histogram(data64, bins=bins64, density=False)

counts64=counts64.astype(float)/data_size64

# Find the cdf
cdf64 = np.cumsum(counts64)

# Plot the cdf
plt.plot(bin_edges64[0:-1], cdf64,linestyle='--', color='g')

print(mean(data64))

data32 = np.loadtxt('scan_32.txt')

data_size32=len(data32)

# Set bins edges
data_set32=sorted(set(data32))
bins32=np.append(data_set32, data_set32[-1]+1)

# Use the histogram function to bin the data
counts32, bin_edges32 = np.histogram(data32, bins=bins32, density=False)

counts32=counts32.astype(float)/data_size32

# Find the cdf
cdf32 = np.cumsum(counts32)

# Plot the cdf
plt.plot(bin_edges32[0:-1], cdf32,linestyle='--', color='r')

print(mean(data32))

data16 = np.loadtxt('scan_16.txt')

data_size16=len(data16)

# Set bins edges
data_set16=sorted(set(data16))
bins16=np.append(data_set16, data_set16[-1]+1)

# Use the histogram function to bin the data
counts16, bin_edges16 = np.histogram(data16, bins=bins16, density=False)

counts16=counts16.astype(float)/data_size16

# Find the cdf
cdf16 = np.cumsum(counts16)

# Plot the cdf
plt.plot(bin_edges16[0:-1], cdf16,linestyle='--', color='c')

print(mean(data16))

data8 = np.loadtxt('scan_8.txt')

data_size8=len(data8)

# Set bins edges
data_set8=sorted(set(data8))
bins8=np.append(data_set8, data_set8[-1]+1)

# Use the histogram function to bin the data
counts8, bin_edges8 = np.histogram(data8, bins=bins8, density=False)

counts8=counts8.astype(float)/data_size8

# Find the cdf
cdf8 = np.cumsum(counts8)

# Plot the cdf
plt.plot(bin_edges8[0:-1], cdf8,linestyle='--', color='k')

print(mean(data8))

data4 = np.loadtxt('scan_4.txt')

data_size4=len(data4)

# Set bins edges
data_set4=sorted(set(data4))
bins4=np.append(data_set4, data_set4[-1]+1)

# Use the histogram function to bin the data
counts4, bin_edges4 = np.histogram(data4, bins=bins4, density=False)

counts4=counts4.astype(float)/data_size4

# Find the cdf
cdf4 = np.cumsum(counts4)

# Plot the cdf
plt.plot(bin_edges4[0:-1], cdf4,linestyle='--', color='y')

print(mean(data4))


data2 = np.loadtxt('scan_2.txt')

data_size2=len(data2)

# Set bins edges
data_set2=sorted(set(data2))
bins2=np.append(data_set2, data_set2[-1]+1)

# Use the histogram function to bin the data
counts2, bin_edges2 = np.histogram(data2, bins=bins2, density=False)

counts2=counts2.astype(float)/data_size2

# Find the cdf
cdf2 = np.cumsum(counts2)

# Plot the cdf
plt.plot(bin_edges2[0:-1], cdf2,linestyle='--', color='m')

print(mean(data2))

data = np.loadtxt('scan_1.txt')

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
plt.plot(bin_edges[0:-1], cdf,linestyle='--', color='b')

print(mean(data))

plt.ylim((0,1))
plt.xlim((0,100000)) #use 20 for gets
plt.ylabel("CDF")
plt.xlabel("Service Time (ns)")
plt.legend(["Query Length (256)","Query Length (128)","Query Length (64)", "Query Length (32)","Query Length (16)","Query Length (8)","Query Length (4)","Query Length (2)","Query Length (1)"], loc ="lower right")
plt.title("Range Query Service Time Distribution")
plt.grid(True)

plt.axvline(mean(data256), color='r')
plt.axvline(mean(data128), color='b')
plt.axvline(mean(data64), color='g')
plt.axvline(mean(data32), color='r')
plt.axvline(mean(data16), color='c')
plt.axvline(mean(data8), color='k')
plt.axvline(mean(data4), color='y')
plt.axvline(mean(data2), color='m')
plt.axvline(mean(data), color='b')

plt.show()
