# Bundled fonts

Inter 4.1 by Rasmus Andersson and the Inter Project Authors (https://rsms.me/inter/), licensed under the
SIL Open Font License 1.1 (see `Inter-OFL.txt`). Inter has no Reserved Font Name.

The files are subsets of `extras/ttf/` from the official `Inter-4.1.zip` release, cut down to Latin, Greek,
Cyrillic, punctuation and common symbols with fontTools (`pyftsubset`), keeping the default OpenType features plus
`tnum`, `case` and `zero`. Text in other scripts falls back to Windows' system fonts. They are embedded in
`NEXUS.exe` as RCDATA resources and loaded privately at run time; nothing is installed system-wide.

Regenerate with:

    for f in Inter-Regular Inter-SemiBold InterDisplay-SemiBold; do
      pyftsubset extras/ttf/$f.ttf --output-file=$f.ttf \
        --unicodes="U+0000-024F,U+0259,U+02B0-02FF,U+0300-036F,U+0370-03FF,U+0400-04FF,U+1E00-1EFF,U+2000-206F,U+20A0-20CF,U+2100-214F,U+2190-21FF,U+2200-22FF,U+2300-23FF,U+25A0-25FF,U+2600-26FF,U+2713-2717,U+FB01-FB02,U+FEFF,U+FFFD" \
        --layout-features+=tnum,case,zero --name-IDs='*' --name-languages='*' --notdef-outline --glyph-names=false
    done
