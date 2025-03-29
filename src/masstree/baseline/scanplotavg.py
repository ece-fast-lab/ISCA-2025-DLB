import numpy as np
import matplotlib.pyplot as plt
from statistics import mean


#plt.ylim((0,1))
#plt.xlim((0,50000)) #use 20 for gets
plt.ylabel("Avg Service Time (ns)")
plt.xlabel("Range Query Length")
#plt.legend(["Query Length (64)", "Query Length (32)","Query Length (16)","Query Length (8)","Query Length (4)","Query Length (2)","Query Length (1)"], loc ="lower right")
plt.title("Range Query Service Time vs. Query Length")
plt.grid(True)

x = [1,2,4,8,16,32,64,128,256]
y = [2200.09079807,2528.69535898,3141.01343642,5338.39539309,8241.16321678,14350.038579,24047.3879867,43378.9921545,83870.8148309]
plt.plot(x,y)

plt.show()
