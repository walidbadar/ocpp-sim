# Fonts

`Montserrat-Bold-latin.ttf` is Montserrat Bold from
<https://github.com/JulietaUla/Montserrat> (`fonts/ttf/Montserrat-Bold.ttf`),
reduced to the characters the display uses:

```sh
pyftsubset Montserrat-Bold.ttf \
    --unicodes='U+0020-007E,U+00B0,U+00B7,U+00D7,U+2013,U+2026' \
    --layout-features='' --no-hinting --desubroutinize --name-IDs='*' \
    --output-file=Montserrat-Bold-latin.ttf
```

Copyright 2024 The Montserrat.Git Project Authors. Licensed under the SIL Open
Font License, Version 1.1: see `OFL.txt`.
