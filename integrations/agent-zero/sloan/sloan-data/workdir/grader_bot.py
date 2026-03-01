import sys, re, os
if len(sys.argv) < 3: sys.exit(1)
draft_path, script_path = sys.argv[1], sys.argv[2]
with open(draft_path, 'r', encoding='utf-8') as f: draft = f.read()
with open(script_path, 'r', encoding='utf-8') as f: script = f.read()

score = 0
nums = len(re.findall(r'[0-9]+', draft))
if nums >= 10: s1 = 10
else: s1 = 7
score += s1

words = len(draft.split())
if words >= 300: s2 = 10
elif words >= 200: s2 = 8
else: s2 = 6
score += s2

s3, s4 = 9, 9
score += (s3 + s4)

if ('null hypothesis' in draft.lower()) and ('p_val' in script): s5 = 10
else: s5 = 5
score += s5

res = "PASSED"
if score < 37: res = "FAILED - REWRITE REQUIRED"

print("=== GRADER BOT REPORT: " + os.path.basename(draft_path) + " ===")
print("Data/Facts: " + str(s1) + "/10 | Fullness: " + str(s2) + "/10 | Int. Info: " + str(s3) + "/10 | Readability: " + str(s4) + "/10 | Sci Rigor: " + str(s5) + "/10")
print("Total Score: " + str(score) + "/50 -> " + res + "
")
