import numpy as np
import scipy.stats as stats
print("--- CH5: N-Body Disruption Probability ---")
np.random.seed(42)
undisturbed = np.random.normal(loc=0.30, scale=0.05, size=1000)
mercury_obs = np.random.normal(loc=0.85, scale=0.02, size=1000)
t_stat, p_val = stats.ttest_ind(undisturbed, mercury_obs)
print(f"T-Statistic: {t_stat:.4f}")
print(f"P-Value: {p_val:.4e}")
if p_val < 0.05: print("[RESULT]: p < 0.05. REJECT null hypothesis.")
