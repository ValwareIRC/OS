# Facet 2.1

Facet is the language the OS's AI writes desktops and whole apps in. Facet 2 grows it from desktop markup
into an app language: components, state, live data from the system, logic in its own small language, and
patches to running apps. Looks always come from the person's own design.

This spec was designed and agreed by a council of six models, chosen so that small local models as well as
large ones can write it: GLM-5.3, GLM-4.5-Air, Mistral-Nemo 12B, Qwen3 14B, Claude Haiku and Claude Opus
(chair). Each proposed a language, then they voted on 18 contested choices. Each then wrote a real app in
the draft (Files, Notes, Todo, App Launcher, Stopwatch) and named everything it had to guess. The final
version folds in every condition they raised, and all six agreed to it.

It is a spec. The interpreter that runs it in the OS is not built yet.

```
Facet is the language the OS's AI writes whole apps in. One file is one app. Code lives in keyword sections closed by `end`; markup lives only inside `view` and always closes with `</tag>` or `/>`. There is exactly one way to write each thing.

CLOSERS, ONE RULE: `end` closes code blocks (app, state, derived, fn, if, for, view, style, component). `</tag>` closes markup. Every markup tag starts with `<`. Never mix them.

FILE SHAPE (sections in this order; every section is optional except view; fn and component may repeat)
  app Name
    data ...            one line each
    state ... end
    derived ... end
    fn name(args) ... end
    view <markup> end
    style <rules> end
    component Name(params) ... end
  end

DATA: one line each, live from the system, read-only. It may use state names: it is read again when they change, and after this app's own fs.* changes.
  data items = files(folder)                 every entry; files(folder, ".txt") only that ending. Records: name, path, size, modified, dir
  data note = file(path)                     record: name, path, text ("" if the file doesn't exist)
  data accent = setting("design.accent")     a value
  data apps = apps()                         records: id, name
  data now = clock(1000)                     ticks every 1000 ms: time (milliseconds since 1970: subtract two for a
                                             duration), hour, minute, second, ms (within the second), day, weekday
STATE: names with starting values; only `set` changes them.
DERIVED: names computed from data, state and other derived; recomputed when what they read changes. May call pure fns (fns with no set and no effects; `facet check` verifies).

STATEMENTS (only inside fn; fns may read every data, state and derived name)
  let name = expr                 a local: only in fn, until the fn ends; shadowing a state, data or derived name is an error
  set name = expr                 changes a state name or a local (writing `name = expr` without set is an error naming the fix)
  if expr then ... elseif expr then ... else ... end
  for x in expr do ... end
  return expr
  name(args)                      call another fn
  emit "event" value              component -> parent: <Row on:event="fn(value)"/>
  EFFECTS (fn only): fs.read(path) -> text, fs.write(path, text), fs.remove(path),
    fs.rename(path, newpath), fs.mkdir(path), app.open(appid), app.open(appid, path), app.close(),
    app.notify(text), ask(question) -> true/false, prompt(question, default) -> text or none
    (ask and prompt draw a dialog in the person's design; the fn waits for the answer)
  TIMERS: after("name", ms, fnname)  every("name", ms, fnname)  cancel("name")
    the third argument is the fn's NAME, not a call: after("save", 1000, save)
    scheduling the same name again restarts it

EXPRESSIONS
  literals: "text" (may contain {expr}; numbers inside render as text), 12, 1.5, true, false, none,
            lists [a, b], records {name: "x", n: 2}
  names, a.b, list[i] (from 0), calls f(x), lambdas only as arguments: n -> expr
  map, filter and find also take (x, i) -> expr: i is x's position in the list they were given, from 0
  conditional: if c then a else b   (an expression: no end, both branches required)
  + on numbers adds; + on texts joins; + on lists joins them: laps + [x]; text + number is an error ("use text(n)")
  PRECEDENCE: the usual arithmetic (* / % before + -), then comparisons (== != < > <= >=),
  then `not`, then `and`/`or`. `not` applies to one comparison or name: `not a.done`, `not (a == b)`.
  Mixing `and` with `or` needs parentheses; the error prints the corrected line.
STDLIB (pure; slices count from 0, end-exclusive)
  len lower upper trim split(text, sep) join(list, sep) text(x) number(text) pad(text, width, char)
  round(x) (halves away from zero) floor(x) ceil(x) min(a, b) max(a, b)
  slice(list|text, start, end) (end optional; clamped to 0..len; start after end gives empty; a negative number
  is an error suggesting len(x) - n) first(list) last(list) (none when empty)
  sort(list, x -> key) filter(list, x -> test) map(list, x -> value) find(list, x -> test) (none if no match)
  contains(text|list, x) starts(text, prefix) ends(text, suffix) date(time, "%H:%M") (local time) base(path) dirname(path)

VIEW (one root element)
  elements: row column grid stack panel text button input field checkbox icon image spacer divider scroll
            canvas popover sheet, and components by name <NoteRow note={n}/>
  text content: <text>Lap {l.n}</text>, <button>Delete</button>: words and {expr} as the element's content
  attributes: a literal "x", a number 12, or an expression {expr}; "Delete {n.name}?" mixes them
  id="search": author-written; unique among the elements of one item or one view; addressing it in a patch
    inside <each> changes it in every item. Needed only where a patch or style must find it.
  events: always a call: on:click="pick(n)"  on:input="typed(value)"  on:change="changed(value)"
    on:key="pressed(value)": value is up down left right enter esc tab backspace delete space home end, or the
    typed character
  bind="draft": two-way with a state name (input, field, checkbox); runs before that element's on:input
  checked={expr}: a checkbox showing data one way, changed through on:change="fn(value)"; never with bind
  more: placeholder="Search"  disabled={running}  autofocus  multiline (on field)
  class="row" (fixed) and class:active={open == n} (when true)
  <each in={shown} as="n" key={n.path} at="i"> ... </each>   key required and unique; at optional (from 0)
  <if test={open != none}> ... <else/> ... </if>   (an empty branch is fine)

STYLE (scoped to the app; looks come from the person's design)
  selectors: #id .class element :hover :focus :active and combinations "#side .row:hover"
  properties, exactly these names:
    background  color  border  radius  padding  margin  gap  width  height  grow
    font  size  weight  shadow  blur  opacity  align  justify  direction  transition
  colours, fonts and shadows MUST be tokens: token(accent) token(surface) token(text) token(muted) token(danger)
  token(font-body)...; a literal colour is an error that lists this design's tokens. Sizes are plain numbers.

COMPONENTS: component Row(note, selected) ... end with the same sections (no data). Ids inside it belong to each
  copy. It talks to its parent only with emit.

ERRORS: caught when the file loads, before anything is drawn: file, line, the offending text, and the corrected
  line when there is one:
    notes.facet:41: unknown fn "sav" in on:input — did you mean "save"?   on:input="save(value)"
  `facet check` reports every error at once; `facet fmt` writes the one canonical layout.
  No eval, no names built from text, no macros, no silent coercion.

PATCHING A RUNNING APP (validated first, rejected whole on any error; never re-runs data, state or fns; state
  survives)
  [[facet:patch notes]]
  set #id attr = value
  style #id { prop: value; }
  add #parent first|last <markup/>
  move #id into|before|after #other
  replace #id <markup/>
  remove #id
  fn name(args) ... end        (replaces that fn)
  [[/facet]]
  Whole app: [[facet:app notes]] the full file [[/facet]]

A WHOLE EXAMPLE (the shape every app follows)
  app Notes
    data notes = files("~/Notes", ".txt")
    state
      query = ""
      open = none
      draft = ""
    end
    derived
      shown = filter(notes, n -> contains(lower(n.name), lower(query)))
    end
    fn pick(n)
      set open = n
      set draft = fs.read(n.path)
    end
    fn typed()
      after("save", 1000, save)
    end
    fn save()
      if open != none then
        fs.write(open.path, draft)
      end
    end
    fn remove()
      if ask("Delete {open.name}?") then
        cancel("save")
        fs.remove(open.path)
        set open = none
      end
    end
    view
      <row>
        <column id="side">
          <input id="search" bind="query" placeholder="Search" autofocus/>
          <each in={shown} as="n" key={n.path}>
            <button class="note" class:active={open == n} on:click="pick(n)">{n.name}</button>
          </each>
        </column>
        <if test={open != none}>
          <column id="editor">
            <field id="body" multiline bind="draft" on:input="typed()"/>
            <button id="delete" on:click="remove()">Delete</button>
          </column>
        <else/>
          <text id="empty">Pick a note</text>
        </if>
      </row>
    end
    style
      #side { width: 240; gap: 4; }
      .note.active { background: token(accent); }
      #delete { color: token(danger); }
    end
  end
```

## What 2.1 added

The final round of 2.0 left five open questions. The chair proposed answers and all six members accepted
them unanimously:

- `let` for local names inside a fn.
- `checked={expr}` on a checkbox.
- `(x, i) ->` in `map`, `filter` and `find`. `sort` keeps the one-name form.
- `slice` clamps at the ends, and negative numbers are an error.
- `clock()` gives `time` in milliseconds since 1970, so durations don't wrap at midnight.

## Looks (added after 2.1, to put to the council)

Facet 2.1 gave apps only plain style properties, so an app with little style looked plain. These additions give a writer, small models included, beauty with a few words. None of it is a stock look: each class is computed from the person's design. That covers its material (glass, frost, solid or neon), corners, shadows, glow, colours and fonts. So `class="card lift"` is frosted in one design, neon-edged in another and paper in a third.

**Built-in classes** (combine freely: `class="card lift center"`):

| | |
|---|---|
| Surfaces | `card` `tile` `glass` `hero` (accent gradient banner) `sunken` `outline` `raised` `floating` |
| Emphasis | `primary` `soft` `quiet` `danger` `success` `glow` |
| Shape | `pill` `circle` |
| Type | `title` `heading` `label` (small, upper case, muted) `subtle` `big` (a number to stand out) `mono` |
| Layout | `spread` `center` `left` `stack` `wrap` `fill` `grid2` `grid3` `grid4` `tight` `airy` |
| Motion | `lift` (rises on hover, as fast as the design moves) |

**More style properties:**
- `gradient: token(accent) token(accent2) 135` paints the background.
- `glow: token(accent) 14` makes it give off light.
- `depth: 0..3` sets this design's shadow levels (light on neon).
- `scale: 1.05` resizes it, and `move: 0 -3` shifts it (both most useful on `:hover`).
- `text: start|center|end` aligns its text.
- `upper: true` sets its text in capitals.
- `line: 1.4` sets the line height.
- `columns: 3` lays its children out as a grid.
- `outline: token(border)` draws a border.

**Widgets,** drawn in the design's accent flowing into accent2, easing to new values:
- `<ring value={done} max={goal} label="water"/>`
- `<progress value={read} max={pages}/>`
- `<chart kind="bar|line" data={list} labels={list}/>`
- `<avatar text="Valerie Liu"/>` (initials)
- `<badge>3 new</badge>`

`tests/facet2/daybook.facet` shows them all. `tests/facet2/showcase.sh` pictures it in several designs.

## The network (added after 2.1, agreed with Valerie)

An app asks once for the internet, as a single permission for the whole app, and the answer is remembered. After that:

- **Live web data:** `data weather = web("https://...", 600000)` gives the reply's JSON (records and lists), or `none` until it arrives. It is fetched again every 600000 ms. It may follow state, like other data.
- **Requests in a fn:** `let r = net.get(url)` and `let r = net.post(url, body)`. A record or list body is sent as JSON, a text as text. Both give `{ok, status, data, text, error}`. The fn waits, and the OS goes on meanwhile.
- **Sockets:** `set conn = net.connect(host, port, mode, heard)`.
  - `mode` is `"tcp"` or `"tls"`, then `"lines"` (text a line at a time, without `\r\n`) or `"bytes"` (lists of numbers 0..255).
  - `fn heard(conn, kind, data)` hears each event: `open`, `line`, `bytes`, `closed` or `error`.
  - `net.send(conn, text or bytes)` and `net.close(conn)`.
- **JSON both ways:** `json(value)` and `parse(text)`.

`tests/facet2/chat.facet` is an IRC-style client and `tests/facet2/weather.facet` uses live JSON. Both run in `facet run` against a local server and canned replies.
