import os, subprocess
print("====== THE HUMAN FRACTION: EXECUTION PIPELINE ======")
models = ['ch3_real_nlp.py']
for m in models:
    if os.path.exists('/a0/usr/workdir/' + m):
        print(f">>> EXECUTING: {m}")
        subprocess.run(['python3', '/a0/usr/workdir/' + m])
print("================ PIPELINE COMPLETE =================")
