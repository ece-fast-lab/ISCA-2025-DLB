import numpy as np
import matplotlib.pyplot as plt
from statistics import mean

"""
data128 = np.loadtxt('replace_128.txt')

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

plt.axvline(mean(data128), color='b')
print(mean(data128))



data64 = np.loadtxt('replace_64.txt')

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
plt.axvline(mean(data64), color='g')
print(mean(data64))

data32 = np.loadtxt('replace_32.txt')

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
plt.axvline(mean(data32), color='r')
print(mean(data32))

data16 = np.loadtxt('replace_16.txt')

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
plt.axvline(mean(data16), color='c')
print(mean(data16))

data8 = np.loadtxt('replace_8.txt')

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
plt.axvline(mean(data8), color='k')
print(mean(data8))

data4 = np.loadtxt('replace_4.txt')

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
plt.axvline(mean(data4), color='y')
print(mean(data4))



data2 = np.loadtxt('replace_2.txt')

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
plt.axvline(mean(data2), color='m')
print(mean(data2))

data11 = np.loadtxt('replace_1.txt')

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
plt.plot(bin_edges11[0:-1], cdf11,linestyle='--', color='g')

plt.axvline(mean(data11), color='g')
print(mean(data11))
"""

data = np.loadtxt('replace.txt')

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
plt.axvline(mean(data), color='b')
print(mean(data))

"""
data13 = np.loadtxt('replace_new_3.txt')

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

plt.axvline(mean(data13), color='r')
"""

plt.ylim((0,1))
plt.xlim((0,10000)) #use 20 for gets
plt.ylabel("CDF")
plt.xlabel("Service Time (ns)")
#plt.legend(["Query Length (64)", "Query Length (32)","Query Length (16)","Query Length (8)","Query Length (4)","Query Length (2)","Query Length (1)"], loc ="lower right")
plt.title("Put Query Service Time Distribution")
plt.grid(True)


plt.show()
