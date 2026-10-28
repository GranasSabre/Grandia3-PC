# Extra letters of the patched game font (runtime: text_hook.cpp EXTRA, sys_text.cpp DE).
# Glyphs 0x98.. in this order; font code / script byte = glyph + 0x20 (0xB8..0xD1).
EXTRA = 'äöüÄÖÜß' + 'àâçéèêëîïôùûÀÇÉÈÊÔÂ'
CODE = {ch: 0xB8 + i for i, ch in enumerate(EXTRA)}
# base letter for width measuring
BASE = dict(zip(EXTRA, 'aouAOUB' + 'aaceeeeiiouuACEEEOA'))


def normalize(s):
    """characters the font lacks, replaced like the runtime does"""
    for a, b in (('œ', 'oe'), ('Œ', 'Oe'), ('«', '"'), ('»', '"'), (' ', ' '), (' ', ' ')):
        s = s.replace(a, b)
    return s
