# The companion API: what the OS offers apps

Agreed between the OS and Prism (2026-10-31). Apps on the OS use the companion's AI, identity, face,
voice and memory through the OS, with no setup and no keys. Whatever an app inherits is a default:
it can change anything inside itself.

## Finding it

- The desktop shell makes a socket per app: `$XDG_RUNTIME_DIR/app/<app-id>/os-companion.sock`
  (Prism: `/run/user/1000/app/com.valware.prism/os-companion.sock`). Flatpak mounts that folder into
  the app's sandbox, so no manifest permission or network is needed.
- The path tells the OS which app is asking: per-app consent, nothing to spoof.
- No socket means the app isn't running on this OS.
- The socket speaks HTTP/1.1. AI requests use the OpenAI shapes, everything else is JSON, and events
  arrive as Server-Sent Events on `GET /events`.

## Consent

- The first request asks it once, with one prompt and two lines:
  "Prism wants to **use your AI**" and "**share what I know about you**, both ways".
  Each line can be allowed on its own.
- While the prompt is up, requests are held open for up to 2 minutes, then answered normally if
  allowed. Requests made while it is up all wait on the same prompt.
- `403 {"error":"consent_denied"}` when refused. `403 {"error":"consent_pending"}` only when the prompt
  timed out. `POST /consent/request` asks again; an app does this only when the person asks it to.
- Answers are stored by the OS and can be changed in Settings > Apps.
- Memory endpoints use the memory line; everything else uses the AI line.

## AI

- `POST /v1/chat/completions` (streaming or not) and `GET /v1/models`. The OS forwards each request to
  whichever mind it chose (built-in, local or cloud) and adds nothing to the prompt.
- `/v1/models` lists one model, `"os"`, with a `label` (e.g. "Claude Sonnet (yours)"). For the model on this
  computer it also says `"context"`: how many tokens it can hold, which depends on the memory (4096 to 16384).
- `503 {"error":"no_mind"}` when no mind is set up.
- Function calling: `tools`, `tool_choice`, `tool_calls`, `role:"tool"` messages with `tool_call_id`,
  `image_url` data URLs and `stream_options` pass through untouched to minds that speak OpenAI (the
  built-in model runs with `--jinja` so it takes tools), and are translated for Anthropic and Gemini
  minds. Usage numbers come in the final stream chunk when the mind gives them.
- The "os" entry in `/v1/models` says `"tools":false` when the mind can't call tools (Claude on this
  PC through its bridge, which turns them off); the app then keeps it for chat.

## Identity

- `GET /identity` returns:

      {"name":"Mita","pronouns":"they","persona":"<personality text>",
       "voice":{"id":"en_US-amy","pitch":1.0},
       "vrm":{"version":17,"sha256":"…"},"emblem":"drawing","version":42}

  `pronouns` is only filled if it set them. `emblem` is how the companion looks, a look it and the
  person made together in conversation: `face` (the figure, or the VRM when there is one), `drawing`
  (one it drew of itself), `picture`, `model`, or `none` (no look yet: nothing of its shows). There
  are no built-in looks; an app that can't show its own shows nothing in its place rather than a stand-in.
- `GET /identity/vrm` returns the VRM file, with its sha256 as the ETag. `vrm` is null when the
  companion has no VRM; the app then keeps its own face.
- Changes arrive on `/events` as `identity {"version":n}`.

## The look

How the desktop looks right now, so an app can look like it belongs (colours and shapes, not the scene:
no wallpaper picture). Uses the AI line of consent.

- `GET /look` returns (`404 {"error":"no_design_yet"}` before the first desktop):

      {"version":3,"dark":true,
       "colors":{"bg":"#0b1020","bg2":"#141a33","surface":"#1b2140","surface_text":"#e8ecff","text":"#f2f4ff",
                 "muted":"#9aa3c7","accent":"#7aa2ff","accent2":"#ff8ad8"},
       "surface_opacity":0.72,"blur":14.0,"radius":14.0,"shadows":true,"material":"frost","glow":0.40,"motion":0.50,
       "fonts":{"heading":"Fraunces","heading_files":["/usr/share/fonts/Fraunces-Regular.ttf"],
                "body":"Inter","body_files":["/usr/share/fonts/Inter-Regular.ttf","/usr/share/fonts/Inter-Bold.ttf"],
                "heading_px":28.0,"body_px":15.0},
       "wallpaper":{"kind":"aurora","colors":["#…","#…","#…","#…"],"intensity":0.60,"animated":true},
       "comfort":{"text_scale":1.00,"high_contrast":false,"reduce_motion":false}}

  - `material` is glass, frost, solid or neon. `surface_opacity` runs 0.3–1, `blur` 0–24 px, and
    `glow` and `motion` 0–1.
  - `wallpaper` only describes it (its kind and colours), for apps that want a matching tint.
  - Font files are paths on the OS. Inside a Flatpak, the same files are normally under
    `/run/host/fonts/` with the part after `/usr/share/fonts/`.
- `/events` sends `look {"version":n}` whenever the companion restyles or redesigns. `hello` carries the
  current `look` version too.
- Windows: the desktop draws app windows' frames in the design's own style. An app that wants that asks for
  server-side decoration (xdg-decoration); the compositor offers it and uses it unless the app asks to
  draw its own.

## The face (both ways)

- On `/events`:
  - `mood {"name"}` and `act {"name"}`, using the same names as the companion's `[[mood:]]` and `[[act:]]`;
  - `speaking {"level","f1","f2"}` every 20 ms while the OS speaks;
  - `gaze {"x","y"}`: where it looks, each -1.2..1.2 from the middle of its frame; x grows to the
    right of the screen and y grows **upwards** (the pointer above it is positive y). Sent while the
    pointer moves, at most 20 times a second.
- `POST /avatar {"mood":"happy"}` or `{"act":"wave"}`: the desktop companion does the same.
- Apps render the VRM themselves; the OS does not send frames.

## Voice

- `POST /voice/speak {"text","voice"?}` joins one queue with the companion's own speech, so they never
  talk over each other. `speaking` events drive every face. The reply waits until the line has been
  spoken: `{"done":true}`, or `{"done":false,"why":"stopped"}` when it was cut off.
- `POST /voice/stop` stops everything this app queued, at once.
- Inside a Flatpak, ordinary sound (Web Audio, PulseAudio at `$XDG_RUNTIME_DIR/pulse/native`) plays,
  but there is no recording through PulseAudio: the microphone is only reached through `/voice/*`.
  Speech should go through `/voice/speak` too, so faces move and voices don't overlap.
- `POST /voice/listen/claim` → `{"granted":true,"token":"…"}`. While an app holds the microphone, the
  companion stops listening and shows that it's in a call.
- With the claim:
  - `GET /voice/listen?token=…` is an SSE stream of `partial` and `final` transcripts (the OS's whisper);
  - `GET /voice/mic?token=…` is raw 16 kHz mono 16-bit PCM, for apps doing their own recognition.
- `/voice/listen` events: `ready`, `speech {"state":"start"|"end"}`, `final {"text"}`, `end`. There is no
  echo cancelling: while the OS is speaking, only speech clearly louder than its voice starts a
  `speech`, and that start says `"over_voice":true`.
- `POST /voice/listen/release` gives the microphone back. Closing the socket releases it too.

## Memory (both ways)

- An entry:

      {"id":"m_8f3a…","text":"Night owl, sharpest after 10pm","kind":"rhythm",
       "at":"2026-10-31T12:00:00Z","source":"onboarding","app":null}

  - `kind` is a learned field (`rhythm`, `protect`, `words`, `voice`, `presence`, `boundaries`,
    `redesign`), `about` (something it said about itself) or `note`.
  - `source` is `onboarding`, `companion`, `person` (they typed it) or `app`, with `app` then holding
    the app id.
- `GET /memory` returns `{"version":n,"entries":[…]}`.
- `GET /memory?since=n` returns only the changes. A forgotten entry comes back as `{"id","deleted":true}`.
- `POST /memory {"text","kind"}` adds an entry; `POST /memory {"id","text"}` corrects one. Either way
  the reply is the stored entry, with its id. The OS stores
  it as if the companion had learned it (it shows in Settings as "from Prism").
- `DELETE /memory/<id>` forgets it.
- Every change, from either side, bumps the version and sends `memory {"version":n}` on `/events`.
- "Forget that" said in either place deletes the entry, so it disappears from both. The OS keeps only
  the id of a forgotten entry, never its text.
