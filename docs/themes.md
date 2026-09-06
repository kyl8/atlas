# Themes

A theme is a Lua file that returns a table. Put personal themes in `%USERPROFILE%/.atlas/themes`; bundled themes are installed in `share/atlas/themes` beside the executable. User themes are checked first, so they can override a bundled name.

The compiled default theme is the final fallback. Missing fields are merged from it, and invalid values produce warnings instead of stopping Atlas.

## Theme file

```lua
return {
    name = "soft",
    author = "you",
    palette = {
        pink = "#F2A7C3",
        blue = "#A2D2FF",
        text = "#FFF1E6"
    },
    styles = {
        primary = { fg = "text" },
        accent = { fg = "pink", bold = true },
        success = { fg = "blue", bold = true },
        path = { fg = "blue" },
        muted = { fg = "#9A8294", dim = true }
    }
}
```

Style fields are `fg`, `bg`, `bold`, `dim`, `italic`, and `underline`. Colors may be `#RRGGBB` or a name from `palette`. The renderer handles terminal capabilities, `NO_COLOR`, plain output, and ANSI fallbacks.

## Identity and art

Themes and `config.lua` can customize the identity and home art:

```lua
atlas.ui.identity = {
    name = "atlas",
    separator = "  ·  ",
    version = "1.0",
    palette = true
}

atlas.ui.art = {
    type = "image",
    source = "D:/Pictures/atlas.png",
    size = "auto",
    backend = "auto"
}
```

Use `type = "ascii"` with `inline` or `logo` for text art, `type = "none"` to disable it, and `width`/`height` for manual image sizing. `atlas.theme_overrides` changes selected styles without copying a complete theme.

```powershell
atlas theme preview soft
```
