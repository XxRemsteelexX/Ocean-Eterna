import numpy as np
import scipy.stats as stats
np.random.seed(42)
surface_survival = np.random.normal(loc=10.0, scale=3.0, size=500)
deep_survival = np.random.normal(loc=60.0, scale=10.0, size=500)
t_stat, p_val = stats.ttest_ind(surface_survival, deep_survival)
