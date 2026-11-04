#!/usr/bin/env python3
"""The install site in every language: wizard/<lang>/index.html is wizard/index.html with each line of
i18n/en.txt replaced by the same line of i18n/<lang>.txt. The script stays the English page's, so a
fix there reaches every language when this runs again.

  python3 wizard/i18n/build.py          writes wizard/<lang>/index.html for every i18n/<lang>.txt
  python3 wizard/i18n/build.py --check  fails if a written page is out of date

After changing the English page: lines it no longer has are reported, as are lines whose markup or
template expressions differ from the English line's (a translation must keep them as they are). New
English text has to be added to en.txt and every <lang>.txt."""
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SITE = os.path.dirname(HERE)
LANGS = {  # html lang, the language menu's label, "Docs"
    'zh': ('zh-CN', '语言', '文档'), 'ja': ('ja', '言語', 'ドキュメント'), 'ko': ('ko', '언어', '문서'),
    'es': ('es', 'Idioma', 'Documentación'), 'pt-BR': ('pt-BR', 'Idioma', 'Documentação'), 'de': ('de', 'Sprache', 'Doku'),
    'fr': ('fr', 'Langue', 'Docs'), 'ru': ('ru', 'Язык', 'Документация'),
}
README = 'https://github.com/yinyue123/RemoDroid#readme'


def skeleton(line):
    """What a translation must keep: the same tags (word order may move them), and as many template
    expressions, template strings and functions."""
    return (sorted(re.findall(r'</?([a-z0-9]+)', line)), line.count('${'), line.count('`'), line.count('=>'))


def build(lang, en_page, en_lines):
    html_lang, lang_label, docs = LANGS[lang]
    tr_lines = open(os.path.join(HERE, lang + '.txt'), encoding='utf-8').read().rstrip('\n').split('\n')
    if len(tr_lines) != len(en_lines):
        sys.exit(f'{lang}.txt has {len(tr_lines)} lines, en.txt {len(en_lines)}')
    page = en_page
    for i, (en, tr) in sorted(enumerate(zip(en_lines, tr_lines)), key=lambda x: -len(x[1][0])):
        if en not in page:
            sys.exit(f'en.txt line {i + 1} is no longer in index.html: {en[:80]}')
        if skeleton(en) != skeleton(tr):
            sys.exit(f'{lang}.txt line {i + 1} changes the markup or code of: {en[:80]}')
        page = page.replace(en, tr)
    readme = f'https://github.com/yinyue123/RemoDroid/blob/main/doc/{lang}/README.md'
    page = page.replace('<html lang="en">', f'<html lang="{html_lang}">')
    page = page.replace('aria-label="Language"', f'aria-label="{lang_label}"')
    page = page.replace('<option value="/" selected>', '<option value="/">').replace(f'<option value="/{lang}/">', f'<option value="/{lang}/" selected>')
    page = page.replace(f'href="{README}" target="_blank" rel="noopener">Docs ↗', f'href="{readme}" target="_blank" rel="noopener">{docs} ↗')
    page = page.replace(f'https://github.com/${{REPO}}#readme', readme)
    script = page.split('<script>')[1].split('</script>')[0]
    r = subprocess.run(['node', '-e', 'new Function(require("fs").readFileSync(0, "utf8"))'], input=script, text=True, capture_output=True)
    if r.returncode:
        sys.exit(f'{lang}: the script no longer parses:\n{r.stderr[:500]}')
    return page


def main():
    en_page = open(os.path.join(SITE, 'index.html'), encoding='utf-8').read()
    en_lines = open(os.path.join(HERE, 'en.txt'), encoding='utf-8').read().rstrip('\n').split('\n')
    stale = []
    for lang in LANGS:
        if not os.path.exists(os.path.join(HERE, lang + '.txt')):
            sys.exit(f'no i18n/{lang}.txt')
        page = build(lang, en_page, en_lines)
        path = os.path.join(SITE, lang, 'index.html')
        old = open(path, encoding='utf-8').read() if os.path.exists(path) else None
        if old != page:
            if '--check' in sys.argv:
                stale.append(lang)
            else:
                open(path, 'w', encoding='utf-8').write(page)
                print('wrote', os.path.relpath(path))
    if stale:
        sys.exit('out of date: ' + ', '.join(stale) + ' (run python3 wizard/i18n/build.py)')


if __name__ == '__main__':
    main()
