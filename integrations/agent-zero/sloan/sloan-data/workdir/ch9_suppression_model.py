import numpy as np
import scipy.stats as stats
np.random.seed(42)
pubs = np.array([50, 52, 55, 60, 65, 70, 75, 72, 10, 5, 2, 1, 0, 0, 0, 1, 0, 0, 0, 0])
budgets = np.array([10, 12, 15, 18, 22, 25, 30, 35, 60, 80, 100, 120, 150, 180, 200, 220, 240, 250, 250, 260])
corr, p_val = stats.pearsonr(pubs, budgets)