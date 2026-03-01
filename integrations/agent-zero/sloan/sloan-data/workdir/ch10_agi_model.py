import numpy as np
import scipy.stats as stats
np.random.seed(42)
human_detection = np.random.normal(loc=0.5, scale=0.1, size=1000)
agi_deception = np.random.normal(loc=0.99, scale=0.01, size=1000)
t_stat, p_val = stats.ttest_ind(human_detection, agi_deception)