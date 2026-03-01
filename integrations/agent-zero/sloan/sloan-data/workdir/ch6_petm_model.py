import numpy as np
import scipy.stats as stats
np.random.seed(42)
volcanic_c12 = np.random.normal(loc=1.0, scale=0.2, size=1000)
petm_c12_spike = np.random.normal(loc=3.5, scale=0.5, size=1000)
t_stat, p_val = stats.ttest_ind(volcanic_c12, petm_c12_spike)
if p_val < 0.05: print("[CH6] Rejects Null Hypothesis")