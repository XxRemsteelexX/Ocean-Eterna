import os
import time
from interpreter import interpreter

# Configure Open Interpreter
os.environ['OPENAI_API_KEY'] = 'dummy_x'
interpreter.auto_run = True
interpreter.llm.model = 'i'

manuscript_path = 'Manuscript_Longform/Chapter_01_The_Shrinking_Year.md'
output_path = 'Manuscript_Longform/Chapter_01_Expanded_Phase2_Test.md'

print("=== Auto Author Pipeline: Phase 2 ===")
print("Loading Chapter 1...")

try:
    with open(manuscript_path, 'r', encoding='utf-8') as f:
        content = f.read()
except FileNotFoundError:
    print(f"Could not find {manuscript_path}. Are you in the right directory?")
    exit(1)

# Extract first section (approx 3 paragraphs)
paragraphs = content.split('\n\n')
chunk_to_expand = '\n\n'.join(paragraphs[:3])

orig_len = len(chunk_to_expand.split())
print(f"Original Chunk Length: {orig_len} words.")
print("Initiating Open Interpreter Free Model ('i') for recursive expansion...")
print("(This may take a moment depending on the public server load...)\n")

prompt = (
    "You are a master scientific author. Expand this brief text into a dense, "
    "highly detailed academic book section. Be wildly descriptive and empirical. "
    "Focus purely on prose. DO NOT write or execute any code. "
    "Just output the expanded prose:\n\n" + chunk_to_expand
)

start_time = time.time()
try:
    # Send prompt to open interpreter
    messages = interpreter.chat(prompt, display=False)

    # Parse response
    if messages and isinstance(messages, list):
        expanded_text = str(messages[-1].get('content', ''))
    else:
        expanded_text = str(messages)

except Exception as e:
    expanded_text = f"Error during generation: {e}"

elapsed = time.time() - start_time
new_len = len(expanded_text.split())

print(f"Expansion operation completed in {elapsed:.1f} seconds.")
print(f"Expanded Chunk Length: {new_len} words.")
print(f"Expansion Ratio: {new_len / max(1, orig_len):.2f}x")

with open(output_path, 'w', encoding='utf-8') as f:
    f.write("# EXPANDED SECTION TEST\n\n")
    f.write(expanded_text)

print(f"\nTest expansion saved successfully to {output_path}\n")
