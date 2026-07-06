Modules architecture
====================

Module (in general) should follow best practices: be mostly independant pieces of code, ensure proper interface surface, do not
expose implementation, preferably buildable on a host for simulation.

Modules should provide interface for init and safe shutdown. All internal state, callback etc should be cleaned upon deinit.

Modules might run own FreeRTOS task/Host thread inside. In that case (besides some special cases) internal state of the task
should be only exposed to public module API with corrent inter-task primitives: queues, events, streams, locks etc.

Top level view
--------------

**Display module:** [Implementation: mature] opens predefined size screen with text framebuffer. Default size 800x480. Hosts a bitmap font 8x16. 
Draws that font with per-cell color attributes fg, bg (possibly bold, under, strike, blink attrs). Manages configurable
cursor overlay. Exposes framebuffer pointer and cursor API. Character is unicode, but for compactness capped to first
16 bit (font has some supported ranges, otherwise output 'default' char).

On device version uses ISR to draw one text line of bounce buffer on each call.

Simulated version uses SDL backed surface and redraws full display at once, reusing drawing code.

Plans:
- addsupport for status quake style icons-overlays (wifi connection, bt ready, disconnected)
- possibly support for compactly encoded (indirect reference) 2ch wide chars, emojis
- visual terminal bell feedback (flash, shake)


**Terminal:** [Implementation: partial] Currently unused. Low level 'DOS' like output over Display module. Handles utf8 to code-point conversion.
Provide simple printing text API. Tracks current color, cursor position. Scrolls on overflow. Should provide input but not implemented yet.

Plans:
- add simple input


**SSH:** [Implementation: need improvements] Wraps libssh2 with convenient interface. Provides synchronous API for connect / disconnect.
Internally runs own task handling i/o and current state. Data should be passed in and out via task-safe methods. Defines interface
for connection presets (host/port/key/password), handling known-hosts and others. Receive path is optimized for faster ingest, send path
optimized for minimal input lag. Trimmed memory appetite for embedded use.
(Handling of ssh activated user interactions are out of scope for now) 

Plans:
- Proper publickey integration


**Storage** [Implementation: basic] Interface to stored user preferences, stored connection profiles, public keys.

On device version is based on littlefs storage.

Simulated version access files on host fs.

**VTerm:** [Implementation: mature, advancing] Main 'virtual terminal' - wraps 'tsm' for processing input and drawing to internal
terminal frame-buffer. Provides api to feed input, interface to setup terminal callbacks: reports, bell, etc. Access 'display' buffer
directly to draw updated regions from own buffer. Tracks cursor.

Plans:
- switch between ssh session io and local io (so they won't mix accidentially)
- add keycode, utf8 char + mods -> terminal escale sequence translation
- improve interface surface

**Wifi:** [Implementation: partial] Initializes Wifi connection. Tracks status.

Plans:
- Add connection profiles, persist current to storage
- Provide assisted onboarding: copy wifi settings from ESP App via BT
- Provide direct onboarding: show visible SSID, pick one, enter password

**Input:** [Implementation: basic] Provides meanings of user input via HAL. Supports on device: BLE keyboard (optionally UART/JTAG keyboard),
Touch screen as mouse. On simulator: real keyboard and mouse. Has low level input interface (key press, mods) as well as higher - utf8 characters.

Plans:
- BLE keyboard pairing
- Register shortcuts (intercept from input) to callbacks (or other comm method)

**Cyberdeck:** [Implementation: none] Orchestrating other modules, io, user UI. Shared between device and simulation.

Plans:
- Initial booting screen
- Peripherals and wifi connection status, setup
- TUI to choose from stored connection profiles
