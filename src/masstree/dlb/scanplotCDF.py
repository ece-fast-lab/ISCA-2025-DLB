import numpy as np
import matplotlib.pyplot as plt

data64 = np.loadtxt('scan_scantest_64.txt')

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
plt.plot(bin_edges64[0:-1], cdf64,linestyle='--', marker="o", color='g')



data32 = np.loadtxt('scan_scantest_32.txt')

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
plt.plot(bin_edges32[0:-1], cdf32,linestyle='--', marker="o", color='r')


data16 = np.loadtxt('scan_scantest_16.txt')

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
plt.plot(bin_edges16[0:-1], cdf16,linestyle='--', marker="o", color='c')



data8 = np.loadtxt('scan_scantest_8.txt')

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
plt.plot(bin_edges8[0:-1], cdf8,linestyle='--', marker="o", color='k')



data4 = np.loadtxt('scan_scantest_4.txt')

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
plt.plot(bin_edges4[0:-1], cdf4,linestyle='--', marker="o", color='y')





data2 = np.loadtxt('scan_scantest_2.txt')

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
plt.plot(bin_edges2[0:-1], cdf2,linestyle='--',  marker="o", color='m')


data = np.loadtxt('scan_scantest_1.txt')

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
plt.xlim((0,10000)) #use 20 for gets
plt.ylabel("CDF")
plt.xlabel("Service Time (ns)")
plt.legend(["Query Length (64)", "Query Length (32)","Query Length (16)","Query Length (8)","Query Length (4)","Query Length (2)","Query Length (1)"], loc ="lower right")
plt.title("Range Query Service Time Distribution")
plt.grid(True)


plt.show()
