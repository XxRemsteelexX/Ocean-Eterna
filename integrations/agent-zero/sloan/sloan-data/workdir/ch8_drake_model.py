import numpy as np
import scipy.stats as stats
np.random.seed(42)
N_sims = 100000
R_star = np.random.normal(2.0, 0.5, N_sims)
f_p = np.random.uniform(0.5, 1.0, N_sims)
n_e = np.random.uniform(0.1, 0.5, N_sims)
f_l = np.random.uniform(0.01, 1.0, N_sims)
f_i = np.random.uniform(0.01, 0.5, N_sims)
f_c = np.random.uniform(0.1, 0.5, N_sims)
L = np.random.lognormal(mean=np.log(10000), sigma=1.0, size=N_sims)
N_civs = R_star * f_p * n_e * f_l * f_i * f_c * L
t_stat, p_val = stats.ttest_1samp(N_civs, popmean=1.0, alternative='greater')