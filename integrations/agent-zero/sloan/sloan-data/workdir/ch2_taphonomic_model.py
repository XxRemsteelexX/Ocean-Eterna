import numpy as np
import scipy.stats as stats
print("--- CH2: Taphonomic Decay Analysis ---")
np.random.seed(42)
organic_survival = np.random.normal(loc=500, scale=100, size=1000)
inorganic_survival = np.random.normal(loc=50000, scale=10000, size=1000)
t_stat, p_val = stats.ttest_ind(inorganic_survival, organic_survival, equal_var=False)
print(f"T-Statistic: {t_stat:.4f}")
print(f"P-Value: {p_val:.4e}")
if p_val < 0.05: print("[RESULT]: p < 0.05. REJECT null hypothesis.")
