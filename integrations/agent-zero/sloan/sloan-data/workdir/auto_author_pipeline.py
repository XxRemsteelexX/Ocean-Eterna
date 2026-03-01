import os
import glob

def init_pipeline():
    print('=== Auto Author Pipeline: Word Count Initialization ===')
    manuscript_dir = 'Manuscript_Longform'
    if not os.path.exists(manuscript_dir):
        print(f'Directory {manuscript_dir} not found. Ensure it exists.')
        return

    total_words = 0
    filepaths = glob.glob(os.path.join(manuscript_dir, '*.md'))
    filepaths.sort()

    if not filepaths:
        print('No markdown files found.')
        return

    for path in filepaths:
        with open(path, 'r', encoding='utf-8') as f:
            content = f.read()
            words = len(content.split())
            total_words += words
            print(f'[{os.path.basename(path)}]: {words:,} words')

    print('-------------------------------------------------------')
    print(f'TOTAL MANUSCRIPT WORDS: {total_words:,}')
    print('-------------------------------------------------------')
    print('Phase 1 Orchestrator Ready.')
    print('Beginning Phase 2: Recursive LLM Expansion...')

if __name__ == '__main__':
    init_pipeline()
