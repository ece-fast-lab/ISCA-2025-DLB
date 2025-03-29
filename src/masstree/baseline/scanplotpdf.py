from scipy.stats import norm
import numpy as np
import matplotlib.pyplot as plt


data = np.loadtxt('scan_scantest_1.txt')


plt.plot(data, norm.pdf(data))
plt.xlim((0,5000)) #use 20 for gets
plt.ylim((0,0.005))
plt.show()
