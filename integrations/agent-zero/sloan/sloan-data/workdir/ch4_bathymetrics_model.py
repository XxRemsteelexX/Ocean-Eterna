import numpy as np
import scipy.stats as stats
print("--- CH4: Bathymetric Discovery Probability ---")
np.random.seed(42)
terrestrial = np.random.normal(loc=15.0, scale=3.0, size=500)
submerged = np.random.normal(loc=0.5, scale=0.1, size=500)
t_stat, p_val = stats.ttest_ind(terrestrial, submerged)
print(f"T-Statistic: {t_stat:.4f}")
print(f"P-Value: {p_val:.4e}")
if p_val < 0.05: print("[RESULT]: p < 0.05. REJECT null hypothesis.")
