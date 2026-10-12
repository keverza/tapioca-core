# Roof designer typography

Inter is the body/control font. Space Grotesk is the regular heading font.
Both are embedded DATA resources so the roof editor does not substitute machine
fonts or download anything at runtime. Other tools keep their existing font.

Source: Google Fonts `ofl/inter` and `ofl/spacegrotesk`, fetched 2026-10-12 from
`https://github.com/google/fonts`. Original SIL Open Font License files accompany
the original TTFs. Inter's default instance is weight 400. Space Grotesk's
upstream variable default is weight 300, so `SpaceGrotesk.ttf` is the regular
weight-400 static instance generated with fontTools 4.66.1. The unmodified
upstream input is retained as `SpaceGrotesk-Variable.ttf` for reproducibility.

```python
from fontTools.ttLib import TTFont
from fontTools.varLib.instancer import instantiateVariableFont
font = TTFont("SpaceGrotesk-Variable.ttf")
instantiateVariableFont(font, {"wght": 400}, updateFontNames=True).save("SpaceGrotesk.ttf")
```
