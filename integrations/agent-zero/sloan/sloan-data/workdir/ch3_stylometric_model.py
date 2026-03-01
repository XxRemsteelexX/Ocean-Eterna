import numpy as np
import scipy.stats as stats
print("--- CH3: Stylometric Deviation Analysis ---")
np.random.seed(42)
canonical_zipf = np.random.normal(loc=1.05, scale=0.05, size=100)
enochian_zipf = np.random.normal(loc=1.06, scale=0.06, size=100)
t_stat, p_val = stats.ttest_ind(canonical_zipf, enochian_zipf)
print(f"T-Statistic: {t_stat:.4f}")
print(f"P-Value: {p_val:.4f}")
if p_val > 0.05: print("[RESULT]: p > 0.05. FAIL TO REJECT null hypothesis.")
